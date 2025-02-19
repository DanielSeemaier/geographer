/*
 * MeshGenerator.cpp
 *
 *  Created on: 22.11.2016
 *      Author: tzovas
 */

#include "MeshGenerator.h"
#include <chrono>

#include <scai/common/macros/assert.hpp>

using std::string;
using std::list;
using std::ifstream;
using std::istream_iterator;
using std::ofstream;
using std::endl;
using std::istringstream;

using scai::hmemo::HArray;
using scai::hmemo::ReadAccess;
using scai::hmemo::WriteAccess;
using scai::hmemo::WriteOnlyAccess;


namespace ITI {

//-------------------------------------------------------------------------------------------------
// coords.size()= 3 , coords[i].size()= N
// here, N= numPoints[0]*numPoints[1]*numPoints[2]

//TODO: some code duplication with its 2D counterpart, create a core function and 2D/3D wrappers?

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::createStructured3DMesh_seq(CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, std::vector<ValueType> maxCoord, std::vector<IndexType> numPoints) {
    SCAI_REGION( "MeshGenerator.createStructured3DMesh" )

    if (coords.size() != 3) {
        throw std::runtime_error("Needs three coordinate vectors, one for each dimension");
    }

    if (numPoints.size() != 3) {
        throw std::runtime_error("Needs three point counts, one for each dimension");
    }

    std::vector<ValueType> offset= {maxCoord[0]/numPoints[0], maxCoord[1]/numPoints[1], maxCoord[2]/numPoints[2]};
    IndexType N= numPoints[0]* numPoints[1]* numPoints[2];
    // create the coordinates
    IndexType index=0;
    {
        SCAI_REGION("createStructured3DMesh.setCoordinates");
        for( IndexType indX=0; indX<numPoints[0]; indX++) {
            for( IndexType indY=0; indY<numPoints[1]; indY++) {
                for( IndexType indZ=0; indZ<numPoints[2]; indZ++) {
                    coords[0].setValue(index, indX*offset[0] );
                    coords[1].setValue(index, indY*offset[1] );
                    coords[2].setValue(index, indZ*offset[2] );
                    ++index;
                }
            }
        }
    }

    scai::lama::CSRStorage<ValueType> localMatrix;
    localMatrix.allocate( N, N );

    //create the adjacency matrix
    HArray<IndexType> csrIA;
    HArray<IndexType> csrJA;
    HArray<ValueType> csrValues;
    // ja and values have size= edges of the graph
    // for a 3D structured grid with dimensions AxBxC the number of edges is 3ABC-AB-AC-BC
    IndexType numEdges= 3*numPoints[0]*numPoints[1]*numPoints[2]
                        - numPoints[0]*numPoints[1]
                        - numPoints[0]*numPoints[2]
                        - numPoints[1]*numPoints[2];
    {
        WriteOnlyAccess<IndexType> ia( csrIA, N +1 );
        WriteOnlyAccess<IndexType> ja( csrJA, numEdges*2);
        WriteOnlyAccess<ValueType> values( csrValues, numEdges*2);
        ia[0] = 0;

        IndexType nnzCounter = 0; // count non-zero elements
        // for every node= for every line of adjM
        for(IndexType i=0; i<N; i++) {
            // connect the point with its, at most, 6 (in 3D) neighbours
            // neighbour_node: the index of a neighbour of i, can take negative values
            // but in that case we do not add it
            IndexType ngb_node = 0;
            IndexType numRowElems= 0;

            std::tuple<IndexType, IndexType, IndexType> thisPoint = aux<IndexType,ValueType>::index2_3DPoint( i, numPoints);

            {
                SCAI_REGION("createStructured3DMesh.setAdjacencyMatrix");
                // for all 6 possible neighbours
                for(IndexType m=0; m<6; m++) {
                    switch(m) {
                    case 0:
                        ngb_node= i+1;
                        break;
                    case 1:
                        ngb_node= i-1;
                        break;
                    case 2:
                        ngb_node = i + numPoints[2];
                        break;
                    case 3:
                        ngb_node = i - numPoints[2];
                        break;
                    case 4:
                        ngb_node = i + numPoints[2]*numPoints[1];
                        break;
                    case 5:
                        ngb_node = i - numPoints[2]*numPoints[1];
                        break;
                    }

                    if(ngb_node>=0 && ngb_node<N) {
                        std::tuple<IndexType, IndexType, IndexType> ngbPoint = aux<IndexType,ValueType>::index2_3DPoint( ngb_node, numPoints);

                        // we need to check distance for the nodes at the outer borders of the grid: eg:
                        // in a 4x4 grid with 16 nodes {0, 1, 2, ...} , for p1= node 3 there is an edge with
                        // p2= node 4 that we should not add (node 3 has coords (0,3) and node 4 has coords (1,0)).
                        // A way to avoid that is check if they are close enough.
                        // TODO: maybe find another, faster way to avoid adding that kind of edges

                        if(dist3DSquared( thisPoint, ngbPoint) <= 1)
                        {
                            ja[nnzCounter]= ngb_node;
                            values[nnzCounter] = 1;         // unweighted edges
                            ++nnzCounter;
                            ++numRowElems;
                        }
                    }
                }
            }
            ia[i+1] = ia[i] +static_cast<IndexType>(numRowElems);
            SCAI_ASSERT_GT_ERROR(ia[i+1], ia[i], "Probably IndexType overflow");
        }//for
        SCAI_ASSERT_EQUAL_ERROR(numEdges*2, ia[ia.size()-1] )
    }
    SCAI_ASSERT_EQUAL_ERROR(numEdges*2, csrValues.size() )
    SCAI_ASSERT_EQUAL_ERROR(numEdges*2, csrJA.size() )

    localMatrix.swap( csrIA, csrJA, csrValues );
    adjM.assign(localMatrix);
}
//-------------------------------------------------------------------------------------------------

//TODO: code duplication as in createGraphStructured3DMesh_seq
template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::writeGraphStructured3DMesh_seq( std::vector<IndexType> numPoints, const std::string filename) {
    SCAI_REGION( "MeshGenerator.writeStructured3DMesh" )

    if (numPoints.size() != 3) {
        throw std::runtime_error("Needs three point counts, one for each dimension");
    }
    std::ofstream f(filename);
    if(f.fail())
        throw std::runtime_error("File "+ filename+ " failed.");

    {   //not distributed
        scai::dmemo::CommunicatorPtr comm = scai::dmemo::Communicator::getCommunicatorPtr();
        SCAI_ASSERT_EQ_ERROR( comm->getSize(), 0, "Function " << __FUNCTION__ <<  " is not distributed, it works only for p=1.");
    }

    IndexType N= numPoints[0]* numPoints[1]* numPoints[2];
    IndexType numEdges= 3*numPoints[0]*numPoints[1]*numPoints[2] - numPoints[0]*numPoints[1]\
                        -numPoints[0]*numPoints[2] - numPoints[1]*numPoints[2];

    // write number of vertices and edges
    f<< N << " " << numEdges << std::endl;

    IndexType nnzCounter = 0; // count non-zero elements
    // for every node= for every line of adjM
    for(IndexType i=0; i<N; i++) {
        IndexType ngbIndex = 0;

        std::tuple<IndexType, IndexType, IndexType> thisPoint = aux<IndexType,ValueType>::index2_3DPoint( i, numPoints);

        {
            SCAI_REGION("writeStructured3DMesh.setAdjacencyMatrix");
            // for all 6 possible neighbours
            for(IndexType m=0; m<6; m++) {
                switch(m) {
                case 0:
                    ngbIndex= i+1;
                    break;
                case 1:
                    ngbIndex= i-1;
                    break;
                case 2:
                    ngbIndex = i + numPoints[2];
                    break;
                case 3:
                    ngbIndex = i - numPoints[2];
                    break;
                case 4:
                    ngbIndex = i + numPoints[2]*numPoints[1];
                    break;
                case 5:
                    ngbIndex = i - numPoints[2]*numPoints[1];
                    break;
                }

                if(ngbIndex>=0 && ngbIndex<N) {
                    std::tuple<IndexType, IndexType, IndexType> ngbPoint = aux<IndexType,ValueType>::index2_3DPoint( ngbIndex, numPoints);

                    // we need to check distance for the nodes at the outer borders of the grid: eg:
                    // in a 4x4 grid with 16 nodes {0, 1, 2, ...} , for p1= node 3 there is an edge with
                    // p2= node 4 that we should not add (node 3 has coords (0,3) and node 4 has coords (1,0)).
                    // A way to avoid that is check if thery are close enough.
                    // TODO: maybe find another, faster way to avoid adding that kind of edges

                    if(dist3DSquared( thisPoint, ngbPoint) <= 1)
                    {
                        f << ngbIndex +1<< " ";
                        ++nnzCounter;
                    }
                }
            }
        }
        f << std::endl;
    }


}

//-------------------------------------------------------------------------------------------------
// coords.size()= 3 , coords[i].size()= N
// here, N= numPoints[0]*numPoints[1]*numPoints[2]

//TODO: code duplication as in its sequential counterpart

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::createStructured2D3DMesh_dist(CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, const std::vector<ValueType> maxCoord, const std::vector<IndexType> numPoints) {
    SCAI_REGION( "MeshGenerator.createStructured2D3DMesh_dist" )

    if (coords.size() != 3) {
        throw std::runtime_error("Needs three coordinate vectors, one for each dimension");
    }

    if (numPoints.size() != 3) {
        throw std::runtime_error("Needs three point counts, one for each dimension");
    }

    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const scai::dmemo::CommunicatorPtr comm = dist->getCommunicatorPtr();
    
    if( !dist->isEqual( coords[0].getDistribution() ) ) {
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and coordinates dist: "<< coords[0].getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    std::vector<ValueType> offset= {maxCoord[0]/numPoints[0], maxCoord[1]/numPoints[1], maxCoord[2]/numPoints[2]};

    // create the coordinates

    // get the local part of the coordinates vectors
    std::vector<HArray<ValueType>* > localCoords(3);

    for(IndexType i=0; i<3; i++) {
        localCoords[i] = &coords[i].getLocalValues();
    }

    IndexType localSize = dist->getLocalSize(); // the size of the local part

    IndexType planeSize= numPoints[1]*numPoints[2]; // a YxZ plane

    // find which should be the first local coordinate in this processor
    IndexType startingIndex = dist->local2Global(0);

    IndexType indX = (IndexType) (startingIndex/planeSize) ;
    IndexType indY = (IndexType) ((startingIndex%planeSize)/numPoints[2]);
    IndexType indZ = (IndexType) ((startingIndex%planeSize) % numPoints[2]);
    //PRINT( *comm<< ": " << indX << " ,"<< indY << ", "<< indZ << ", startingIndex= "<< startingIndex);

    for(IndexType i=0; i<localSize; i++) {
        SCAI_REGION("MeshGenerator.createStructured2D3DMesh_dist.setCoordinates");

        (*localCoords[0])[i] = indX*offset[0];
        (*localCoords[1])[i] = indY*offset[1];
        (*localCoords[2])[i] = indZ*offset[2];
        //PRINT( *comm << ": "<< (*localCoords[0])[i] << "_ "<< (*localCoords[1])[i] << "_ "<< (*localCoords[2])[i]);

        ++indZ;

        if(indZ >= numPoints[2]) {  // if z coord reaches maximum, set it to 0 and increase y
            indZ = 0;
            ++indY;
        }
        if(indY >= numPoints[1]) {  // if y coord reaches maximum, set it to 0 and increase x
            indY = 0;
            ++indX;
        }

        if(indX >= numPoints[0]) {  // reached end of grid
            assert(i == localSize - 1);
        }
    }
    // finish setting the coordinates


    // start making the local part of the adjacency matrix
    scai::lama::CSRStorage<ValueType> localMatrix;
    localMatrix.allocate( adjM.getLocalNumRows(), adjM.getLocalNumColumns() );

    HArray<IndexType> csrIA;
    HArray<IndexType> csrJA;
    HArray<ValueType> csrValues;
    // ja and values have size= edges of the graph
    // for a 3D structured grid with dimensions AxBxC the number of edges is 3ABC-AB-AC-BC
    // for 2D, thus C=1, this results to 2AB-A-B
    IndexType numEdges= 3*numPoints[0]*numPoints[1]*numPoints[2] - numPoints[0]*numPoints[1]\
                        -numPoints[0]*numPoints[2] - numPoints[1]*numPoints[2];

    {
        SCAI_REGION("MeshGenerator.createStructured2D3DMesh_dist.setCSRSparseMatrix");
        IndexType N= numPoints[0]* numPoints[1]* numPoints[2];

        IndexType numNeighbors;

        if(numPoints[2]==1) {	//2D case
            numNeighbors=4;
        } else {					//3D case
            numNeighbors=6;
        }

        WriteOnlyAccess<IndexType> ia( csrIA, adjM.getLocalNumRows() +1 );
        // we do not know the sizes of ja and values. 6*numOfLocalNodes is safe upper bound for a structured 3D mesh
        // after all the values are written the arrays get resized
        WriteOnlyAccess<IndexType> ja( csrJA, numNeighbors*adjM.getLocalNumRows() );
        WriteOnlyAccess<ValueType> values( csrValues, numNeighbors*adjM.getLocalNumRows() );
        ia[0] = 0;
        IndexType nnzCounter = 0; // count non-zero elements

        for(IndexType i=0; i<localSize; i++) {  // for all local nodes
            IndexType globalInd = dist->local2Global(i);    // get the corresponding global index
            // the global id of the neighboring nodes
            IndexType ngb_node = globalInd;
            IndexType numRowElems= 0;     // the number of neighbors for each node. In 2D, corner nodes have only 2 neighbors
            // the position of this node in 3D
            std::tuple<IndexType, IndexType, IndexType> thisPoint = aux<IndexType,ValueType>::index2_3DPoint( globalInd, numPoints);

            // for all possible neighbors
            for(IndexType m=0; m<numNeighbors; m++) {
                switch(m) {
                case 0:
                    ngb_node= globalInd+1;
                    break;
                case 1:
                    ngb_node= globalInd-1;
                    break;
                case 2:
                    ngb_node = globalInd +numPoints[2]*numPoints[1];
                    break;
                case 3:
                    ngb_node = globalInd -numPoints[2]*numPoints[1];
                    break;
                //two last cases are equivalent to the two first for 2D since numPoints[2]=1
                case 4:
                    ngb_node = globalInd +numPoints[2];
                    break;
                case 5:
                    ngb_node = globalInd -numPoints[2];
                    break;
                }

                if(ngb_node>=0 && ngb_node<N) {
                    // get the position in the 3D of the neighboring node
                    std::tuple<IndexType, IndexType, IndexType> ngbPoint = aux<IndexType,ValueType>::index2_3DPoint( ngb_node, numPoints);
                    ValueType distanceSquared = dist3DSquared( thisPoint, ngbPoint);
                    assert(distanceSquared <= numPoints[0]*numPoints[0]+numPoints[1]*numPoints[1]+numPoints[2]*numPoints[2]);

                    if(distanceSquared <= 1) {
                        ja[nnzCounter]= ngb_node;
                        values[nnzCounter] = 1;         // unweighted edges
                        ++nnzCounter;
                        ++numRowElems;
                    }
                }
            }
            assert(numRowElems >= 2);

            ia[i+1] = ia[i] + numRowElems;
        }

        SCAI_ASSERT_EQUAL_ERROR(numEdges*2, comm->sum(nnzCounter));
        ja.resize(nnzCounter);
        values.resize(nnzCounter);
    } //read/write block

    {
        SCAI_REGION( "MeshGenerator.createStructured3DMesh_dist.swap_assign" )
        localMatrix.swap( csrIA, csrJA, csrValues );
        adjM.assignDistribute(localMatrix, adjM.getRowDistributionPtr(), adjM.getColDistributionPtr() );
    }
}
//-------------------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::createStructuredMesh_dist(CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, const std::vector<ValueType> maxCoord, const std::vector<IndexType> numPoints, const IndexType dimensions) {

    if(dimensions==3) {
        createStructured2D3DMesh_dist( adjM, coords, maxCoord, numPoints);
    } else if( dimensions==2) {
        std::vector<ValueType> copyMaxCoord(maxCoord);
        copyMaxCoord.push_back(1);
        std::vector<IndexType> copyNumPoints(numPoints);
        copyNumPoints.push_back(1);
        std::vector<DenseVector<ValueType>> copyCoords(coords);
        copyCoords.push_back( scai::lama::DenseVector<ValueType>(coords[0].size(),0) );

        createStructured2D3DMesh_dist( adjM, copyCoords, copyMaxCoord, copyNumPoints);
    } else {
        throw std::runtime_error("Function createStructuredMesh_dist only supports 2 or 3 dimension grids but dimension given is "+ std::to_string(dimensions)\
                                 + ".\nAborting...");
    }
}

//-------------------------------------------------------------------------------------------------
// coords.size()= 3 , coords[i].size()= N
// here, N= numPoints[0]*numPoints[1]*numPoints[2]

//TODO: not used, deprecate?
//TODO: similarities with an rgg generator?

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::createRandomStructured3DMesh_dist(CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, std::vector<ValueType> maxCoord, std::vector<IndexType> numPoints) {
    SCAI_REGION( "MeshGenerator.createRandomStructured3DMesh_dist" )

    if (coords.size() != 3) {
        throw std::runtime_error("Needs three coordinate vectors, one for each dimension");
    }

    if (numPoints.size() != 3) {
        throw std::runtime_error("Needs three point counts, one for each dimension");
    }

    const scai::dmemo::DistributionPtr dist = adjM.getRowDistributionPtr();
    const scai::dmemo::CommunicatorPtr comm = dist->getCommunicatorPtr();

    if( !dist->isEqual( coords[0].getDistribution() ) ) {
        std::cout<< __FILE__<< "  "<< __LINE__<< ", matrix dist: " << *dist<< " and coordinates dist: "<< coords[0].getDistribution() << std::endl;
        throw std::runtime_error( "Distributions: should (?) be equal.");
    }

    //for simplicity
    IndexType numX = numPoints[0], numY= numPoints[1], numZ= numPoints[2];
    IndexType N = numX* numY* numZ;

    std::vector<ValueType> offset= {maxCoord[0]/numX, maxCoord[1]/numY, maxCoord[2]/numZ};

    // create the coordinates

    // get the local part of the coordinates vectors
    std::vector<HArray<ValueType>* > localCoords(3);

    for(IndexType i=0; i<3; i++) {
        localCoords[i] = &coords[i].getLocalValues();
    }

    IndexType localSize = dist->getLocalSize(); // the size of the local part

    IndexType planeSize= numY*numZ;             // a YxZ plane

    // find which should be the first local coordinate in this processor
    IndexType startingIndex = dist->local2Global(0);

    IndexType indX = (IndexType) (startingIndex/planeSize) ;
    IndexType indY = (IndexType) ((startingIndex%planeSize)/numZ);
    IndexType indZ = (IndexType) ((startingIndex%planeSize) % numZ);
    //PRINT( *comm<< ": " << numX << ", "<< numY << ", "<< numZ << ", startingIndex= "<< startingIndex);

    for(IndexType i=0; i<localSize; i++) {
        SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.setCoordinates");

        (*localCoords[0])[i] = indX*offset[0];
        (*localCoords[1])[i] = indY*offset[1];
        (*localCoords[2])[i] = indZ*offset[2];
        //PRINT( *comm << ": "<< (*localCoords[0])[i] << "_ "<< (*localCoords[1])[i] << "_ "<< (*localCoords[2])[i]);

        ++indZ;

        if(indZ >= numZ) {  // if z coord reaches maximum, set it to 0 and increase y
            indZ = 0;
            ++indY;
        }
        if(indY >= numY) {  // if y coord reaches maximum, set it to 0 and increase x
            indY = 0;
            ++indX;
        }
        if(indX >= numX) {  // reached and of grid
            assert(i == localSize - 1);
        }
    }
    // finish setting the coordinates
    // this part is the same as the structured mesh

    /*
     * start making the local part of the adjacency matrix
    */

    // first find the indices of possible neighbors

    // the diameter of the box in which we gonna pick indices.
    // we set it as: 1 + 2% of the minimum side length. this is >2 for sides >50
    IndexType boxRadius = 1 + (IndexType) (0.02 * (ValueType) *std::min_element(std::begin(numPoints), std::end(numPoints)));

    // radius^3 is the number of possible neighbors, here keep their indices as an array
    std::vector<IndexType> neighbourGlobalIndices;

    //PRINT(*comm << " , boxRadius= "<< boxRadius<< " , and num of possible neighbours ="<< pow(2*boxRadius+1, 3) );

    for(IndexType x=-boxRadius; x<=boxRadius; x++) {
        for(IndexType y=-boxRadius; y<=boxRadius; y++) {
            for(IndexType z=-boxRadius; z<=boxRadius; z++) {
                IndexType globalNeighbourIndex= x*planeSize + y*numZ + z;
                neighbourGlobalIndices.push_back( globalNeighbourIndex );
            }
        }
    }
    //PRINT(*comm<<", num of neighbours inserted= "<< neighbourGlobalIndices.size() );

    // an upper bound to how many neighbours a vertex can have,
    // at most as many neighbours as we have
    IndexType ngbUpperBound = std::min( IndexType(12), (IndexType) neighbourGlobalIndices.size() ); // TODO: I do not know, just trying 12
    // TODO:  maybe treat nodes in the faces differently
    IndexType ngbLowerBound = 3;

    /*  We must the adjacency matrix symmetric and also we do not know how many edges the graph will
     *  have eventually and we cannot use ia, ja and values arrays to build the CSRSparseMatrix.
     *  We will store all edges in a vector and build the matrix afterwards.
     *  Also we separate those edges with non-local neighbours so we can set the non-local edge
     *  later.
     */

    // a set for every local node. localNgbs[i] keeps the neighbours of node i that are also local. We use set in order to prevent the insertion of an index multiple times
    std::vector< std::set<IndexType> > localNgbs(localSize);

    // two vector that keep the edges that nees to be communicated with their global indices
    std::vector<IndexType> localNodeInd;
    std::vector<IndexType> nonLocalNodeInd;

    for(IndexType i=0; i<localSize; i++) {                  // for all local nodes
        SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.findNgbrs");
        IndexType thisGlobalInd = dist->local2Global(i);    // get the corresponding global index
        IndexType ngbGlobalInd;                             // the global id of the neighbouring nodes
        //PRINT(*comm << ": i= " << i<< ", "<< thisGlobalInd);
        // the position of this node in 3D
        std::tuple<IndexType, IndexType, IndexType>  thisPoint = aux<IndexType,ValueType>::index2_3DPoint( thisGlobalInd, numPoints);

        // if point is on the faces it will have only 3 edges
        // TODO: not correct, ridge nodes must have >4 edges and face nodes have >5
        if(std::get<0>(thisPoint)== 0 or std::get<1>(thisPoint)== 0 or std::get<2>(thisPoint)== 0) {
            ngbUpperBound =3;
        }
        if(std::get<0>(thisPoint)== numX-1 or std::get<1>(thisPoint)== numY-1 or std::get<2>(thisPoint)== numZ-1) {
            ngbUpperBound =3;
        }

        // get a random number of neighbours between 3 and ngbUpperBound
        IndexType numOfNeighbours;
        if(ngbUpperBound == ngbLowerBound) {        //for nodes on the faces
            numOfNeighbours = ngbUpperBound;
        } else {
            numOfNeighbours = ((int) rand()%(ngbUpperBound- ngbLowerBound) + ngbLowerBound);
        }
        assert( numOfNeighbours < neighbourGlobalIndices.size() );

        // find all the global indices of the neighbours
        std::set<IndexType> ngbGloblaIndSet;

        for(IndexType j=0; j<numOfNeighbours; j++) {
            SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.findNgbrs.findRelativeIndices");

            IndexType relativeIndex=0;
            std::tuple<IndexType, IndexType, IndexType> ngbPoint;
            bool setInsertion = false;

            do {
                // pick a random index (of those allowed) to greate edge
                unsigned long randInd= (unsigned long) rand()%neighbourGlobalIndices.size() ;

                // not 0 to avoid thisGlobalInd == ngbGlobalInd
                while( relativeIndex==0) {
                    randInd= (unsigned long) rand()%neighbourGlobalIndices.size();
                    assert(randInd < neighbourGlobalIndices.size());
                    relativeIndex = neighbourGlobalIndices[ randInd ];
                }
                // neighbour's relative index, relative to thisIndex (this + or - a value)
                relativeIndex = neighbourGlobalIndices[ randInd ];

                // the global neighbour's index. At this point it can be <0 or >N, ensure this not happens
                ngbGlobalInd = thisGlobalInd + relativeIndex;

                // find a suitable ngbGlobalInd: not same as this, not negative, not >N and close enough
                while( /*(ngbGlobalInd==thisGlobalInd) or*/  (ngbGlobalInd<IndexType(0)) or (ngbGlobalInd>= N) ) {
                    // pick new index at random
                    randInd= (unsigned long) rand()%neighbourGlobalIndices.size();
                    relativeIndex = neighbourGlobalIndices[ randInd ];
                    while( relativeIndex==0) {
                        randInd= (unsigned long) rand()%neighbourGlobalIndices.size();
                        assert(randInd < neighbourGlobalIndices.size());
                        relativeIndex = neighbourGlobalIndices[ randInd ];
                    }

                    ngbGlobalInd = thisGlobalInd + relativeIndex;

                }
                assert( ngbGlobalInd < N);
                assert( ngbGlobalInd >= IndexType(0) );

                // so here, the neighbour's global index should be: >0 && <N
                ngbPoint = aux<IndexType,ValueType>::index2_3DPoint( ngbGlobalInd, numPoints);
                // if the two points are too far, recalculate neighbour index
                if( dist3DSquared(thisPoint, ngbPoint)> 3*boxRadius*boxRadius ) {
                    continue;
                }

                // at this point ngbGlobalInd should be valid: >0 && <N && close enough
                // but we may have already inserted

                // insert the index to the set. if it already exists then setInsertion.second = false
                setInsertion = ngbGloblaIndSet.insert(ngbGlobalInd).second;

            } while(setInsertion==false);
            //finally, we inserted a valid (>0 && <N && close enough) neighbour

        } // for(IndexType j=0; j<numOfNeighbours; j++)

        //
        // from here, ngbGloblaIndSet has all the valid global neighbours
        //
        SCAI_ASSERT_EQUAL_ERROR(ngbGloblaIndSet.size(), numOfNeighbours);

        // for all neighbours inserted
        for(typename std::set<IndexType>::iterator it= ngbGloblaIndSet.begin(); it!= ngbGloblaIndSet.end(); ++it ) {
            SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.findNgbrs.separateNonLocal");
            ngbGlobalInd = *it;
            localNgbs[i].insert( ngbGlobalInd );       // store the local edge (i, ngbGlobalInd)

            // for the other/symmetric edge we must check if ngbGlobalInd is local or not
            if( dist->isLocal(ngbGlobalInd) ) {         // if the neighbour is local
                assert( dist->global2Local(ngbGlobalInd) < localSize );
                localNgbs[ dist->global2Local(ngbGlobalInd) ].insert( dist->local2Global(i) );
            } else {                                    // the neighbour is not local
                localNodeInd.push_back( thisGlobalInd );
                nonLocalNodeInd.push_back( ngbGlobalInd );
            }
        }
    } // for(IndexType i=0; i<localSize; i++)
    //PRINT(*comm << ",  num of non-local ngbs= " << nonLocalNodeInd.size() );

    /*
     * communicate the non-local edges that we must set.
     * idea:
     * we have an edge (u,v) where u is local but v is non-local. In the local part of
     * the adjacency matrix we set adjM(u,v)=1 but must also set adjM(v,u)=1 but v
     * is not local. Store in two vectors the indices: in localNodeInd insert u and on
     * nonLocalNodeInd insert v. Now the edge (v,u) is (localNodeInd[i], nonLocalNodeInd[i]).
     * Make two HArrays of the vectors and pass them around. When you receive the corresponding
     * arrays on will not have local indices (is the localNodeInd from another PE) but there
     * might be local indices in the other array, the nonLocalNodeInd from the other PE.
     * Check if there is an index that is local for you. On array arrRcvPossiblyLocalInd
     * there might be a local index. Suppose that arrRcvPossiblyLocalInd[i] is local, that
     * means that you must add the edge (arrRcvPossiblyLocalInd[i], arrRcvNonLocalInd[i]).
     * After you checked the whole array pass on the array you just received, not your own
     * data again, se very pair of arrays localNodeInd and nonLocalNodeInd will be seen
     */
    // build 2 HArrays from vectors to send data to other PEs
    // first round send your own data, then send what you received

    /* The 2 arrays idea does not seem to work. We will use 1 array and edges wil be
     * in indices [0]-[1] , [2]-[3] .... [i]-[i+1]
     */

    SCAI_ASSERT( localNodeInd.size() == nonLocalNodeInd.size(), __FILE__<<", "<< __LINE__<< ": sizes should match" );

    // [2*i] is the local endpoint, [2*i+1] is the non-local endpoint
    scai::hmemo::HArray<IndexType> sendPart(2* localNodeInd.size());
    scai::hmemo::WriteAccess<IndexType> sendPartWrite( sendPart );
    for(IndexType i=0; i< localNodeInd.size(); i++) {
        sendPartWrite[2*i]= localNodeInd[i];
        sendPartWrite[2*i+1]= nonLocalNodeInd[i];
    }
    sendPartWrite.release();
    //PRINT(*comm << ">> " << sendPart.size() );

    // so on the data this PE receives edge [2*i] is local on the sender and [2*i+1] non-local on the sender
    // so now [2*i+1] may be local to this PE
    scai::hmemo::HArray<IndexType> recvPart;

    // this array contains local indices of other PEs.
    //scai::hmemo::HArray<IndexType> arrRcvNonLocalInd;
    // this array might contains local indices of this PE.
    // they must be the same size

    IndexType numPEs = comm->getSize();

    // the size of what you gonna send nad what you will receive
    scai::hmemo::HArray<IndexType> sendSize( numPEs, IndexType(0) );
    scai::hmemo::HArray<IndexType> recvSize( numPEs,  IndexType(0) );

    SCAI_ASSERT_EQUAL_ERROR( recvSize.size(), comm->getSize() );

    for(IndexType round=0; round<numPEs; round++) {
        SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.commSize");
        {
            scai::hmemo::WriteAccess<IndexType> sendSizeWrite( sendSize );
            sendSizeWrite[ comm->getRank()] = sendPart.size();
        }
        //PRINT(*comm <<" _"<< recvSize.size() << " ## "<< sendSize.size() );
        // communicate first the size of waht you will send and what you will receive
        comm->shiftArray( recvSize, sendSize, 1);
        sendSize.swap( recvSize );
    }

    scai::hmemo::ReadAccess<IndexType> readRcvSize( recvSize);


    for(IndexType round=1; round<comm->getSize(); round++) {
        SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.commNonLocalEdges");
        // to find from what PE you receive
        IndexType indexToRcv = (comm->getRank()-round+numPEs)%numPEs;
        IndexType sizeToRcv = readRcvSize[ indexToRcv ];
        //PRINT("round: "<< round <<" , " << *comm << ": receive from PE: "<< indexToRcv << ", size: "<< sizeToRcv);
        recvPart.resize( sizeToRcv );

        scai::hmemo::ReadAccess<IndexType> sendSizeRead (sendSize);
        //PRINT( *comm << " , sendPart.size= " << sendPart.size() );
        // send your local part, receive other PE's part
        comm->shiftArray( recvPart, sendPart, 1);

        // check if recvPart[2*i+1] is local, if it is must add edge [2*i+1]-[2*i]
        // the symmetric edge [2*i]-[2*i+1] is already been set to the PE that send the part
        // Note that send and recv parts should contain global indices
        for(IndexType i=0; i<recvPart.size(); i=i+2) {
            scai::hmemo::ReadAccess<IndexType> recvPartWrite( recvPart );
            if( dist->isLocal(recvPartWrite[i+1]) ) {                      // must add edge
                IndexType localIndex=  recvPartWrite[i+1];
                IndexType nonLocalIndex= recvPartWrite[i];
                // 0< localIndex< globalN but localNgbs.size()= localN, so must convert it to local
                SCAI_ASSERT( dist->global2Local(localIndex) < localNgbs.size(),"too large index: "<< localIndex <<" while size= "<< localNgbs.size() )
                localNgbs[dist->global2Local(localIndex) ].insert( nonLocalIndex );      // indices are global
            }
            // if not local do not do anything
        }

        // the data you received must be passed on, not your own again
        sendPart.resize(recvPart.size());
        sendPart.swap(recvPart);
    }

    /*
     * after gathering all non local indices, set the CSR sparse matrix
     * all the edges to be set should be in localNgbs:
     * localNgbs is a vector with sets, localNgbs.size()= localN and each set localNgbs[i]
     * has stored the neighbours of node i. Care to change between global and local indices
     * when neccessary.
     *
     * remember:
     * std::vector< std::set<IndexType> > localNgbs(localSize);
     */


    scai::lama::CSRStorage<ValueType> localMatrix;
    localMatrix.allocate( adjM.getLocalNumRows(), adjM.getLocalNumColumns() );

    HArray<IndexType> csrIA;
    HArray<IndexType> csrJA;
    HArray<ValueType> csrValues;
    // ja.size() = values.size() = number of edges of the graph

    IndexType nnzCounter = 0; // count non-zero elements

    {
        SCAI_REGION("MeshGenerator.createRandomStructured3DMesh_dist.setCSRSparseMatrix");
        //IndexType globalN= numX* numY* numZ;

        // Summing the size of all sets. This is the number of all edges.
        IndexType nnzValues=0;
        for(IndexType i=0; i<localNgbs.size(); i++) {
            nnzValues += localNgbs[i].size();
        }
        WriteOnlyAccess<IndexType> ia( csrIA, adjM.getLocalNumRows() +1 );
        WriteOnlyAccess<IndexType> ja( csrJA, nnzValues);
        WriteOnlyAccess<ValueType> values( csrValues, nnzValues );
        ia[0] = 0;

        /*TODO:
         * if the 2  assertions never fail then we can save some time
         * since resizing is never done and there is no need to count the non-zero values
         * and the numRowElems
         */


        for(IndexType i=0; i<localSize; i++) {              // for all local nodes
            // for all the neighbours of node i
            IndexType numRowElems= 0;   // should be == localNgbs[i].size()
            for(typename std::set<IndexType>::iterator it= localNgbs[i].begin(); it!=localNgbs[i].end(); ++it) {
                IndexType ngbGlobalInd = *it;       // the global index of the neighbour
                ja[nnzCounter] = ngbGlobalInd;
                values[nnzCounter] = 1;
                SCAI_ASSERT( nnzCounter < nnzValues, __FILE__<<" ,"<<__LINE__<< ": nnzValues not calculated properly")
                ++nnzCounter;
                ++numRowElems;
            }
            ia[i+1] = ia[i] + numRowElems;
            //PRINT(numRowElems << " should be == "<< localNgbs[i].size() );
            SCAI_ASSERT(numRowElems == localNgbs[i].size(),  __FILE__<<" ,"<<__LINE__<<" something is wrong");
        }
        ja.resize(nnzCounter);
        values.resize(nnzCounter);
        //PRINT("nnz afterwards= " << nnzCounter << " should be == "<< nnzValues);
        SCAI_ASSERT_EQUAL_ERROR( nnzCounter, nnzValues);
    } //read/write block


    {
        SCAI_REGION( "MeshGenerator.createRandomStructured3DMesh_dist.swap_assign" )
        localMatrix.swap( csrIA, csrJA, csrValues );
        adjM.assignDistribute(localMatrix, adjM.getRowDistributionPtr(), adjM.getColDistributionPtr() );
    }

}

//-------------------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::createQuadMesh( CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, const int dimension, const IndexType numberOfAreas, const IndexType pointsPerArea, const ValueType maxVal, const IndexType seed) {
    SCAI_REGION("MeshGenerator.createQuadMesh")

    //Point<ValueType> minCoord(dimension, 0); //something like that does not work, look at quad/Point.h

    Point<ValueType> minCoord(dimension);
    Point<ValueType> maxCoord(dimension);
    for(int i=0; i< dimension; i++) {
        minCoord[i]= 0;
        maxCoord[i]= maxVal;
    }

    IndexType capacity = 1;

    // the quad tree
    QuadTreeCartesianEuclid<ValueType> quad(minCoord, maxCoord, true, capacity);

    // create points and add them in the tree
    std::random_device rd;
    std::default_random_engine generator( seed );
    std::vector<std::normal_distribution<ValueType>> distForDim(dimension);

    std::cout<< "Creating graph for " << numberOfAreas << " areas and " << pointsPerArea << " points per area." <<std::endl;

    for(IndexType n=0; n<numberOfAreas; n++) {
        SCAI_REGION("MeshGenerator.createQuadMesh.addPointsInQuadtree")
        Point<ValueType> randPoint(dimension);

        for(int d=0; d<dimension; d++) {
            std::uniform_real_distribution<ValueType> dist(minCoord[d], maxCoord[d]);
            randPoint[d] = dist(generator);
            // create a distribution for every dimension
            //TODO: maybe also pick deviation in random
            ValueType deviation = ((ValueType) rand()/RAND_MAX +1)*3;
            distForDim[d] = std::normal_distribution<ValueType> (randPoint[d], deviation);
        }

        for(int i=0; i<pointsPerArea; i++) {
            Point<ValueType> pInRange(dimension);
            for(int d=0; d< dimension; d++) {
                ValueType thisCoord = distForDim[d](generator)+ (ValueType) rand()/RAND_MAX;
                // if it is out of bounds pick again
                while(thisCoord<=minCoord[d] or thisCoord>=maxCoord[d]) {
                    thisCoord = distForDim[d](generator)+ (ValueType) rand()/RAND_MAX ;
                }
                assert(thisCoord > minCoord[d]);
                assert(thisCoord < maxCoord[d]);
                pInRange[d] = thisCoord;
            }
            quad.addContent(0,pInRange);
        }
    }

    // add random points to keep tree balanced
    for(IndexType i=0; i<pointsPerArea*2; i++) {
        SCAI_REGION("MeshGenerator.createQuadMesh.randomPoints")
        Point<ValueType> p(dimension);
        for(int d=0; d<dimension; d++) {
            std::uniform_real_distribution<ValueType> dist(minCoord[d], maxCoord[d]);
            p[d] = dist(generator);
            //p[d]= ((ValueType) rand()/RAND_MAX) * maxCoord[d];
        }
        quad.addContent(0, p);
    }

    quad.indexSubtree(0);
    graphFromQuadtree(adjM, coords, quad);
}
//----------------------------------------------------------------------------------------------

template<typename IndexType, typename ValueType>
void MeshGenerator<IndexType, ValueType>::graphFromQuadtree(CSRSparseMatrix<ValueType> &adjM, std::vector<DenseVector<ValueType>> &coords, const QuadTreeCartesianEuclid<ValueType> &quad) {

    const IndexType treeSize = quad.countNodes();
    const IndexType dimension = quad.getDimensions();

    // the quad tree is created. extract it as a CSR matrix
    // graphNgbrsCells is just empty now
    std::vector< std::set<std::shared_ptr<const SpatialCell<ValueType>>> > graphNgbrsCells( treeSize );
    std::vector<std::vector<ValueType>> coordsV( dimension );

    adjM = quad.template getTreeAsGraph<IndexType>( graphNgbrsCells, coordsV );
    const IndexType n = adjM.getNumRows();
    assert(n == coordsV[0].size());

    // copy from vector to DenseVector
    for(int d=0; d<dimension; d++) {
        SCAI_REGION("MeshGenerator.createQuadMesh.copyToDenseVector");
        HArray<ValueType> localValues(n, coordsV[d].data());
        coords[d] = DenseVector<ValueType>(localValues);
    }
}

//----------------------------------------------------------------------------------------------
/* Creates random points in the cube [0,maxCoord] in the given dimensions.
 */
template<typename IndexType, typename ValueType>
std::vector<DenseVector<ValueType>> MeshGenerator<IndexType, ValueType>::randomPoints(IndexType numberOfPoints, int dimensions, ValueType maxCoord) {
    SCAI_REGION( "MeshGenerator.randomPoints" )
    IndexType n = numberOfPoints;
    int d, j;
    std::vector<DenseVector<ValueType>> ret(dimensions);
    for (d=0; d<dimensions; d++)
        ret[d] = DenseVector<ValueType>(n, 0);

    for(d=0; d<dimensions; d++) {
        for(j=0; j<n; j++) {
            ValueType r = ((ValueType) rand()/RAND_MAX) * maxCoord;
            ret[d].setValue(j, r);
        }
    }
    return ret;
}

//-------------------------------------------------------------------------------------------------
template<typename IndexType, typename ValueType>
ValueType MeshGenerator<IndexType, ValueType>::dist3DSquared(std::tuple<IndexType, IndexType, IndexType> p1, std::tuple<IndexType, IndexType, IndexType> p2) {
    SCAI_REGION( "MeshGenerator.dist3DSquared" )
    ValueType distX, distY, distZ;

    distX = std::get<0>(p1)-std::get<0>(p2);
    distY = std::get<1>(p1)-std::get<1>(p2);
    distZ = std::get<2>(p1)-std::get<2>(p2);

    ValueType distanceSquared = distX*distX+distY*distY+distZ*distZ;
    return distanceSquared;
}

//-------------------------------------------------------------------------------------------------

template class MeshGenerator<IndexType, double>;
template class MeshGenerator<IndexType, float>;

//template double MeshGenerator<long int, double>::distSquared( const std::vector<long int> p1, const std::vector<long int> p2);

} //namespace ITI
