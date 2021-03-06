#include <fstream>
#include <iostream>
#include <algorithm>
#include <iterator>
#include <string>
#include <map>
#include <vector>
#include "Graph.h"
#include "MapFunctions.h"
#include "StringFunctions.h"

namespace BrainGraph {

  void Graph::calculate_node_importance(BrainGraph::ROItoROI r2r,
				   std::string prior,
				   std::string count,
				   std::string confidence) {
    // Reset all count and confidence to 0
    for (auto& node : this->nodes_) {
      node.properties[count] = Property(0);
      node.properties[confidence] = Property(0);
    }
    double num_of_paths = 0;
    for (auto path : r2r) {
      ++num_of_paths;
      double length = path.size() - 1.;
      if(length > 0){
    	  auto node_0_weight = std::log(std::sqrt(this->node(path[0].id).properties[prior][0]));
    	  auto node_n_weight = std::log(std::sqrt(this->node(path.back().id).properties[prior][0]));
    	  auto distance = path.back().weight - node_0_weight - node_n_weight;
    	  auto score = std::exp(-(distance/length));
    	  for (auto d_node : path) {
    		  auto& node = this->node(d_node.id);
    		  node.properties[count][0] += 1;
    		  node.properties[confidence][0] += score;
    	  }
      }
    }
  }

  std::vector<property_type> Graph::as_matrix(id_type rows, 
					      id_type columns, 
					      id_type slices,
					      std::string weight_key,
					      std::string pos_key) const {
    std::vector<property_type> weights(rows*columns*slices, 0.0);
    for (id_type i = 0; i < this->no_of_nodes(); ++i) {
      const Property& pos = this->node(i).properties.at(pos_key);
      weights.at(pos[2] + pos[1] * slices + pos[0] * columns * slices) = this->node(i).properties.at(weight_key)[0];
    }
    return weights;
  }

  //
  // Binary save/load
  //

  struct EdgeInfo {
    id_type source, target;
    weight_type weight;
  };
    
  bool Graph::save_binary(std::string path) const {
    std::ofstream out(path);

    if (!out) {
      std::cerr << "Error opening " << path << "\n";
      return false;
    }
    
    // Make the header
    out << "# Header\n"
	<< "node_count = " << this->no_of_nodes() << "\n"
	<< "edge_count = ";
    // We do not know the edge count yet.
    auto edge_count_pos = out.tellp();
    out << std::string(ceil(8*sizeof(id_type)/log2(10)), ' ') << "\n"
	<< "node_id_bytes = " << sizeof(id_type) << "\n"
	<< "property_element_bytes = " << sizeof(property_type) << "\n"
	<< "edge_weight_bytes = " << sizeof(weight_type) << "\n";
      
    if (this->no_of_nodes() > 0) { 
      auto num_props = this->node(0).properties.size();
      if (num_props > 0) {
	out << "# Properties\n"
	    << "properties = ";
	std::size_t i = 0;
	for (auto prop : this->node(0).properties) {
	  out << "(" << prop.first << ":" << prop.second.dim() << ")";
	  if (++i < num_props) {
	    out << ",";
	  }  
	}
	out << "\n";
      }
    }
    
    // Make the data section
    out << "# Data\n";
    // Write Nodes
    for (id_type i = 0; i < this->no_of_nodes(); ++i) {
      auto id = this->node(i).id();
      out.write(reinterpret_cast<char*>(&id), sizeof(id));
      for (auto prop : this->node(i).properties) {
	out.write(reinterpret_cast<char*>(&prop.second.values[0]), prop.second.dim() * sizeof(property_type));
      }
    }
    
    // Write edges
    EdgeInfo e_info;
    auto ptr_e_info = reinterpret_cast<char*>(&e_info);
    std::size_t edge_count = 0;
    for (id_type i = 0; i < this->no_of_nodes(); ++i) {
      e_info.source = i;
      for (auto edge : this->edges(i)) {
	e_info.target = edge.node;
	e_info.weight = edge.weight;
	out.write(ptr_e_info, sizeof(e_info));
	++edge_count;
      }
    }
    
    // Update the edge_count
    out.seekp(edge_count_pos);
    out << edge_count;
    return bool(out);
  }


  // The datatypes are a bit to smart. 
  struct Header {
    id_type node_count;
    id_type edge_count;
    unsigned int node_id_bytes;
    unsigned int property_element_bytes;
    unsigned int total_property_elements;
    unsigned int edge_weight_bytes;
    std::vector< std::pair<std::string, unsigned int> > properties;
  };


  Header parse_header(std::ifstream &file) {
    using namespace StringFunctions;
    // Loops forever on bas input...
    Header header;
    std::string line;
    while (file.good() && line != "# Data") {
      std::getline(file, line);
      if (line == "# Header") {
	// We are expecting 5 entries
	std::map<std::string, std::size_t> header_entries;
	for (int i = 0; i < 5; ++i) {
	  std::getline(file, line);
	  auto mapping = split(line, '=');
	  header_entries[trim(mapping[0])] = std::stoul(trim(mapping[1]));
	}
	header.node_count = header_entries["node_count"];
	header.edge_count = header_entries["edge_count"];
	header.node_id_bytes = header_entries["node_id_bytes"];
	header.property_element_bytes = header_entries["property_element_bytes"];
	header.edge_weight_bytes = header_entries["edge_weight_bytes"];
      }
      else if (line == "# Properties") {
	std::size_t total_elements = 0;
	std::getline(file, line);
	std::size_t start = line.find_first_of('(')+1;
	std::size_t end = line.find_last_of(')');
	line = line.substr(start, end-start);
	for (auto token : split(line, ',')) {
	  auto kv = split(token, ':');
	  std::size_t elements = std::stoul(trim(kv[1], " )"));
	  total_elements += elements;
	  header.properties.push_back({trim(kv[0], " ("), elements});
	}
	header.total_property_elements = total_elements;
      }
    }
    return header;
  }

  // This is pretty messy. There is size information both in the header and in
  // datatypes, f.ex header.property_element_bytes and Property::value_type
  std::pair< std::vector<Node>, std::vector<std::vector<Edge> > > parse_data(std::istream &stream, Header header) {
    std::vector<Node> nodes;
    nodes.reserve(header.node_count);
    std::size_t property_bytes = sizeof(Property::value_type) * header.total_property_elements;

    // It is assumed that all nodes has the same number and type of properties
    std::vector<Property::value_type> property_values(header.total_property_elements);

    for (id_type i = 0; i < header.node_count; ++i) {
      id_type id;
      stream.read(reinterpret_cast<char*>(&id), sizeof(id));
      stream.read(reinterpret_cast<char*>(&property_values[0]), property_bytes);

      // Create the properties by slicing the property_values
      std::map<std::string, Property> properties;
      auto start = property_values.begin();
      for (auto prop_info : header.properties) {
	auto end = start + prop_info.second;
	properties[prop_info.first] = Property(std::vector<property_type>(start, end));
	start = end;
      }
      nodes.push_back(Node(i, properties));
    }

    std::vector< std::vector<Edge> > edges(header.node_count);
    std::size_t edge_bytes = 2 * sizeof(id_type) + sizeof(weight_type);
    for (std::size_t i = 0; i < header.edge_count; ++i) {
      EdgeInfo e_info;
      stream.read(reinterpret_cast<char*>(&e_info), edge_bytes);      
      edges[e_info.source].push_back(Edge(e_info.target, e_info.weight));
    }

    return std::make_pair(nodes, edges);
  }


  Graph Graph::load_binary(std::string path) {
    std::ifstream file(path);
    if (! file.good()) {
      std::cerr << "Could not open file " << path << "\n";
      throw std::runtime_error("file is not good\n");
    }
    auto header = parse_header(file);

    if (header.node_id_bytes != sizeof(id_type)) {
      throw std::runtime_error("Size of node id type is not equal to size of id_type");
    }
    if (header.property_element_bytes != sizeof(property_type)) {
      throw std::runtime_error( "Size of property element type is not equal to size of property_type");
    }
    if (header.edge_weight_bytes > sizeof(weight_type)) {
      throw std::runtime_error( "Size of edge weight type is not equal to size of weight_type");
    }

    // file should now be at the start of the data section
    auto N_and_E = parse_data(file, header);
    return Graph(N_and_E.first, N_and_E.second);
  }

}
