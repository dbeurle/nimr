/*
 * For licensing please refer to the LICENSE.md file
 */

#include "GmshReader.hpp"
#include "GmshReaderException.hpp"

#include <algorithm>
#include <boost/container/flat_set.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/numeric.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

#include <json/json.h>

namespace gmsh
{
Reader::Reader(std::string const& fileName, NodalOrdering ordering, IndexingBase base)
    : fileName(fileName),
      useZeroBasedIndexing(base == IndexingBase::Zero),
      useLocalNodalConnectivity(ordering == NodalOrdering::Local)
{
    gmshFile.precision(sizeof(double));
    fillMesh();
}

void Reader::fillMesh()
{
    auto const start = std::chrono::high_resolution_clock::now();

    gmshFile.open(fileName.c_str());

    if (!gmshFile.is_open())
    {
        throw GmshReaderException("Filename " + fileName + " is not valid");
    }

    std::string token, null;

    std::vector<std::string> physicalNames;

    // Loop around file and read in keyword tokens
    while (!gmshFile.eof())
    {
        gmshFile >> token;

        if (token == "$MeshFormat")
        {
            float gmshVersion; // File format version
            short dataType;    // Precision

            gmshFile >> gmshVersion >> dataType >> null;
            checkSupportedGmsh(gmshVersion);
        }
        else if (token == "$PhysicalNames")
        {
            std::string physicalName;

            int physicalIds = 0;
            gmshFile >> physicalIds;

            for (auto i = 0; i < physicalIds; ++i)
            {
                int dimension = 0, physicalId = 0;
                gmshFile >> dimension >> physicalId >> physicalName;

                // Extract the name from the quotes
                physicalName.erase(remove(physicalName.begin(), physicalName.end(), '\"'),
                                   physicalName.end());
                physicalGroupMap.emplace(physicalId, physicalName);
            }
            token.clear();
            gmshFile >> token;
        }
        else if (token == "$Nodes")
        {
            int nodeIds = 0;
            gmshFile >> nodeIds;
            nodeList.resize(nodeIds);

            for (auto& node : nodeList)
            {
                gmshFile >> node.id >> node.coordinates[0] >> node.coordinates[1] >>
                    node.coordinates[2];
            }
        }
        else if (token == "$Elements")
        {
            int elementIds = 0;
            gmshFile >> elementIds;

            for (int elementId = 0; elementId < elementIds; elementId++)
            {
                int id = 0, numberOfTags = 0, elementTypeId = 0;

                gmshFile >> id >> elementTypeId >> numberOfTags;

                auto const numberOfNodes = mapElementData(elementTypeId);

                List tags(numberOfTags, 0);
                List nodalConnectivity(numberOfNodes, 0);

                for (auto& tag : tags)
                {
                    gmshFile >> tag;
                }
                for (auto& nodeId : nodalConnectivity)
                {
                    gmshFile >> nodeId;
                }

                auto const physicalId = tags[0];

                ElementData elementData(nodalConnectivity, tags, elementTypeId, id);

                // Update the total number of partitions on the fly
                number_of_partitions = std::max(elementData.maxProcessId(), number_of_partitions);

                // Copy the element data into the mesh structure
                meshes[{physicalGroupMap[physicalId], elementTypeId}].push_back(elementData);

                if (elementData.isSharedByMultipleProcesses())
                {
                    for (int i = 4; i < tags[2] + 3; ++i)
                    {
                        auto const ownership = std::make_pair(tags[3], -tags[i]);

                        if (interfaceElementMap.find(ownership) != interfaceElementMap.end())
                        {
                            for (auto const& nodeId : elementData.nodalConnectivity())
                            {
                                interfaceElementMap[ownership].emplace(nodeId);
                            }
                        }
                        else
                        {
                            std::set<int> interfaceNodes(elementData.nodalConnectivity().begin(),
                                                         elementData.nodalConnectivity().end());

                            interfaceElementMap.emplace(ownership, interfaceNodes);
                        }
                    }
                }
            }
        }
    }
    gmshFile.close();

    std::cout << std::string(2, ' ') << "A total number of " << number_of_partitions
              << " partitions was found\n";

    auto const end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Mesh data structure filled in " << elapsed_seconds.count() << "s\n";
}

int Reader::mapElementData(int const elementTypeId)
{
    // Return the number of local nodes per element
    switch (elementTypeId)
    {
        case LINE2: return 2; break;
        case TRIANGLE3: return 3; break;
        case QUADRILATERAL4: return 4; break;
        case TETRAHEDRON4: return 4; break;
        case HEXAHEDRON8: return 8; break;
        case PRISM6: return 6; break;
        case PYRAMID5: return 5; break;
        case LINE3: return 3; break;
        case TRIANGLE6: return 6; break;
        case QUADRILATERAL9: return 9; break;
        case TETRAHEDRON10: return 10; break;
        case HEXAHEDRON27: return 27; break;
        case PRISM18: return 18; break;
        case PYRAMID14: return 14; break;
        case POINT: return 1; break;
        case QUADRILATERAL8: return 8; break;
        case HEXAHEDRON20: return 20; break;
        case PRISM15: return 15; break;
        case PYRAMID13: return 13; break;
        case TRIANGLE9: return 19; break;
        case TRIANGLE10: return 10; break;
        case TRIANGLE12: return 12; break;
        case TRIANGLE15: return 15; break;
        case TRIANGLE15_IC: return 15; break;
        case TRIANGLE21: return 21; break;
        case EDGE4: return 4; break;
        case EDGE5: return 5; break;
        case EDGE6: return 6; break;
        case TETRAHEDRON20: return 20; break;
        case TETRAHEDRON35: return 35; break;
        case TETRAHEDRON56: return 56; break;
        case HEXAHEDRON64: return 64; break;
        case HEXAHEDRON125: return 125; break;
        default:
            throw GmshReaderException("The elementTypeId " + std::to_string(elementTypeId) +
                                      " is not implemented");
    }
    return -1;
}

void Reader::checkSupportedGmsh(float const gmshVersion)
{
    if (gmshVersion < 2.2)
    {
        throw std::runtime_error("GmshVersion " + std::to_string(gmshVersion) +
                                 " is not supported");
    }
}

void Reader::writeMeshToJson(bool const printIndices) const
{
    for (auto partition = 0; partition < number_of_partitions; ++partition)
    {
        Mesh process_mesh;

        // Find all of the elements which belong to this process
        for (auto const& mesh : meshes)
        {
            // Copy the elements into the process mesh
            for (auto const& element : mesh.second)
            {
                if (element.isOwnedByProcess(partition + 1))
                {
                    process_mesh[mesh.first].push_back(element);
                }
            }
        }

        auto local_global_mapping = fillLocalToGlobalMap(process_mesh);

        auto local_nodes = fillLocalNodeList(local_global_mapping);

        if (useLocalNodalConnectivity)
        {
            reorderLocalMesh(process_mesh, local_global_mapping);
        }

        // Check if this local mesh needs to be converted to zero based indexing
        // then correct the nodal connectivities, the mappings and the nodal and
        // element ids of the data structures
        if (useZeroBasedIndexing)
        {
            for (auto& l2g : local_global_mapping) --l2g;

            for (auto& localNode : local_nodes) --localNode.id;

            for (auto& mesh : process_mesh)
            {
                for (auto& element : mesh.second)
                {
                    element.convertToZeroBasedIndexing();
                }
            }
        }

        writeInJsonFormat(process_mesh,
                          local_global_mapping,
                          local_nodes,
                          partition,
                          number_of_partitions > 1,
                          printIndices);

        std::cout << std::string(2, ' ') << "Finished writing out JSON file for mesh partition "
                  << partition << "\n";
    }
}

List Reader::fillLocalToGlobalMap(Mesh const& process_mesh) const
{
    List local_global_mapping;

    for (auto const& mesh : process_mesh)
    {
        for (auto const& element : mesh.second)
        {
            boost::copy(element.nodalConnectivity(), std::back_inserter(local_global_mapping));
        }
    }

    // Sort and remove duplicates
    boost::sort(local_global_mapping);
    local_global_mapping.erase(std::unique(local_global_mapping.begin(),
                                           local_global_mapping.end()),
                               local_global_mapping.end());
    return local_global_mapping;
}

void Reader::reorderLocalMesh(Mesh& process_mesh, List const& local_global_mapping) const
{
    for (auto& mesh : process_mesh)
    {
        for (auto& element : mesh.second)
        {
            for (auto& node : element.nodalConnectivity())
            {
                auto const found = boost::lower_bound(local_global_mapping, node);

                // Reset the node value to that inside the local ordering with
                // the default of one based ordering
                node = std::distance(local_global_mapping.begin(), found) + 1;
            }
        }
    }
}

std::vector<NodeData> Reader::fillLocalNodeList(List const& local_global_mapping) const
{
    std::vector<NodeData> local_node_list;
    local_node_list.reserve(local_global_mapping.size());

    for (auto const& node_index : local_global_mapping)
    {
        local_node_list.push_back(nodeList[node_index - 1]);
    }
    return local_node_list;
}

void Reader::writeInJsonFormat(Mesh const& process_mesh,
                               List const& localToGlobalMapping,
                               std::vector<NodeData> const& nodalCoordinates,
                               int const processId,
                               bool const isMeshDistributed,
                               bool const printIndices) const
{
    // Write out each file to Json format
    Json::Value event;

    std::string filename = fileName.substr(0, fileName.find_last_of(".")) + ".mesh";

    if (isMeshDistributed) filename += std::to_string(processId);

    std::fstream writer;
    writer.open(filename, std::ios::out);

    // Write out the nodal coordinates
    Json::Value nodeGroup;
    auto& nodeGroupCoordinates = nodeGroup["Coordinates"];

    for (auto const& node : nodalCoordinates)
    {
        Json::Value coordinates(Json::arrayValue);
        for (auto const& xyz : node.coordinates)
        {
            coordinates.append(Json::Value(xyz));
        }
        nodeGroupCoordinates.append(coordinates);

        if (printIndices) nodeGroup["Indices"].append(node.id);
    }
    event["Nodes"].append(nodeGroup);

    for (auto const& mesh : process_mesh)
    {
        Json::Value elementGroup;
        auto& elementGroupNodalConnectivity = elementGroup["NodalConnectivity"];

        for (auto const& element_data : mesh.second)
        {
            Json::Value connectivity(Json::arrayValue);

            for (auto const& node : element_data.nodalConnectivity())
            {
                connectivity.append(node);
            }

            elementGroupNodalConnectivity.append(connectivity);

            if (printIndices) elementGroup["Indices"].append(element_data.id());
        }

        elementGroup["Name"] = mesh.first.first;
        elementGroup["Type"] = mesh.first.second;

        event["Elements"].append(elementGroup);
    }

    if (isMeshDistributed)
    {
        auto& eventLocalToGlobalMap = event["LocalToGlobalMap"];
        for (auto const& l2g : localToGlobalMapping)
        {
            eventLocalToGlobalMap.append(l2g);
        }

        int globalStartId = 0;

        for (auto const& interface : interfaceElementMap)
        {
            const auto masterId = interface.first.first;
            const auto slaveId  = interface.first.second;

            if (masterId < slaveId)
            {
                std::set<int> intersection;

                auto const& v1 = interface.second;
                auto const& v2 = interfaceElementMap.at(std::pair<int, int>(slaveId, masterId));

                std::set_intersection(v1.begin(),
                                      v1.end(),
                                      v2.begin(),
                                      v2.end(),
                                      std::inserter(intersection, intersection.begin()));

                if ((processId == masterId - 1 or processId == slaveId - 1))
                {
                    Json::Value interfaceGroup, nodeIds, globalIds;

                    for (auto const& nodeId : intersection) nodeIds.append(nodeId);

                    interfaceGroup["Master"].append(interface.first.first);
                    interfaceGroup["Value"].append(processId == masterId - 1 ? 1 : -1);
                    interfaceGroup["Slave"].append(interface.first.second);
                    interfaceGroup["NodeIds"].append(nodeIds);
                    interfaceGroup["GlobalStartId"].append(globalStartId);

                    event["Interface"].append(interfaceGroup);
                }
                globalStartId += intersection.size();
            }
        }
        event["NumInterfaceNodes"].append(globalStartId);
    }
    Json::StyledWriter jsonwriter;
    writer << jsonwriter.write(event);
    writer.close();
}
}
