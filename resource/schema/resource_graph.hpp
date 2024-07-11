/*****************************************************************************\
 * Copyright 2014 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, LICENSE)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\*****************************************************************************/

#ifndef RESOURCE_GRAPH_HPP
#define RESOURCE_GRAPH_HPP

#include "resource/schema/resource_data.hpp"
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphml.hpp>
#include <boost/graph/graphviz.hpp>

#include <utility>

namespace Flux {
namespace resource_model {

enum class emit_format_t {
    GRAPHVIZ_DOT,
    GRAPH_ML,
};

using pinfra_t = pool_infra_t resource_pool_t::*;
using pname_t = std::string resource_t::*;
using rname_t = std::string resource_relation_t::*;
using rinfra_t = relation_infra_t resource_relation_t::*;
using resource_graph_t = boost::adjacency_list<boost::vecS,
                                               boost::vecS,
                                               boost::bidirectionalS,
                                               resource_pool_t,
                                               resource_relation_t,
                                               std::string>;

using vtx_infra_map_t = boost::property_map<resource_graph_t, pinfra_t>::type;
using edg_infra_map_t = boost::property_map<resource_graph_t, rinfra_t>::type;
using vtx_t = boost::graph_traits<resource_graph_t>::vertex_descriptor;
using edg_t = boost::graph_traits<resource_graph_t>::edge_descriptor;
using vtx_iterator_t = boost::graph_traits<resource_graph_t>::vertex_iterator;
using edg_iterator_t = boost::graph_traits<resource_graph_t>::edge_iterator;
using out_edg_iterator_t = boost::graph_traits<resource_graph_t>::out_edge_iterator;
using f_edg_infra_map_t = boost::property_map<resource_graph_t, rinfra_t>::type;
using f_vtx_infra_map_t = boost::property_map<resource_graph_t, pinfra_t>::type;
using f_res_name_map_t = boost::property_map<resource_graph_t, pname_t>::type;
using f_rel_name_map_t = boost::property_map<resource_graph_t, rname_t>::type;
using f_vtx_infra_map_t = boost::property_map<resource_graph_t, pinfra_t>::type;
using f_vtx_iterator_t = boost::graph_traits<resource_graph_t>::vertex_iterator;
using f_edg_iterator_t = boost::graph_traits<resource_graph_t>::edge_iterator;
using f_out_edg_iterator_t = boost::graph_traits<resource_graph_t>::out_edge_iterator;

template<class name_map, class graph_entity>
class label_writer_t {
   public:
    label_writer_t (name_map &in_map) : m (in_map)
    {
    }
    void operator() (std::ostream &out, const graph_entity ent) const
    {
        out << "[label=\"" << m[ent] << "\"]";
    }

   private:
    name_map m;
};

class edg_label_writer_t {
   public:
    edg_label_writer_t (f_edg_infra_map_t &idata, subsystem_t &s) : m_infra (idata), m_s (s)
    {
    }
    void operator() (std::ostream &out, const edg_t &e) const
    {
        multi_subsystems_t::iterator i = m_infra[e].member_of.find (m_s);
        if (i != m_infra[e].member_of.end ()) {
            out << "[label=\"" << i->second << "\"]";
        } else {
            i = m_infra[e].member_of.begin ();
            out << "[label=\"" << i->second << "\"]";
        }
    }

   private:
    f_edg_infra_map_t m_infra;
    subsystem_t m_s;
};

}  // namespace resource_model
}  // namespace Flux

#endif  // RESOURCE_GRAPH_HPP

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
