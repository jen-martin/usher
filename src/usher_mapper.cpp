#include "usher_graph.hpp"

tbb::mutex data_lock;

int mapper_body::operator()(mapper_input input) {
    TIMEIT();
    
    if (input.variant_pos >= 0) {
        size_t num_nodes = input.bfs->size();
        std::vector<int8_t> states(num_nodes);
        std::vector<std::vector<int>> scores(num_nodes);

        for (size_t i=0; i<num_nodes; i++) {
            scores[i].resize(4);
            for (int j=0; j<4; j++) {
                scores[i][j] = 0;
            }
            states[i] = 0;
        }

        for (auto l: input.T->get_leaves()) {
            size_t node_idx = (*input.bfs_idx)[l->identifier];
            for (int j=0; j<4; j++) {
                if (j != input.ref_nuc) 
                    scores[node_idx][j] = (int) num_nodes;
            }
        }

        for (auto v: input.variants) {
            size_t pos = std::get<0> (v);
            std::vector<int8_t> nucs = std::get<1> (v);
            std::string nid = (*input.variant_ids)[pos];
            auto iter = std::find(input.missing_samples->begin(), input.missing_samples->end(), nid);
            if (iter == input.missing_samples->end()) {
                size_t idx= (*input.bfs_idx)[nid];
                for (int j=0; j<4; j++) {
                    scores[idx][j] = (int) num_nodes;
                }
                for (auto nuc: nucs) {
                    if (nuc < 4) {
                        scores[idx][nuc] = 0;
                    }
                }
            }
            else {
                auto mutations_iter = input.missing_sample_mutations->begin() + (iter - input.missing_samples->begin());
                if (nucs.size() < 4) {
                    data_lock.lock();
                    mutation m;
                    m.chrom = input.chrom;
                    m.position = input.variant_pos;
                    m.ref_nuc = input.ref_nuc;
                    if (nucs.size() == 4) {
                        m.is_missing = true;
                    }
                    else {
                        m.is_missing = false;
                        for (auto n: nucs) {
                            m.mut_nuc.emplace_back(n);
                        }
                    }
                    (*mutations_iter).emplace_back(m);
                    data_lock.unlock();
                }
            }
        }

        // Sankoff: forward pass
        for (auto it=(*input.bfs).rbegin(); it!=(*input.bfs).rend(); it++) {
            auto node = (*it);
            auto node_idx = (*input.bfs_idx)[node->identifier];

            if (!node->is_leaf()) {
                for (auto child: (*node).children) {
                    auto c_id = child->identifier;
                    auto c_idx = (*input.bfs_idx)[c_id];
                    for (int j=0; j<4; j++) {
                        int min_s = (int) num_nodes+1;
                        for (int k=0; k<4; k++) {
                            int c_s;
                            if (k==j) {
                                c_s = scores[c_idx][k];
                            }
                            else {
                                c_s = scores[c_idx][k]+1;
                            }
                            if (c_s < min_s) {
                                min_s = c_s;
                            }
                        }
                        scores[node_idx][j] += min_s;
                    }
                }
            }
        }

        // Sankoff: backward pass
        for (auto it=(*input.bfs).begin(); it!=(*input.bfs).end(); it++) {
            auto node = (*it);
            auto node_idx = (*input.bfs_idx)[node->identifier];

            int8_t par_state = 0;
            if (node->parent != NULL) {
                auto par = node->parent;
                auto par_id = par->identifier;
                auto par_idx = (*input.bfs_idx)[par_id];
                par_state = states[par_idx];
            }
            else {
                par_state = input.ref_nuc;
            }

            int8_t state = par_state;
            int min_s = scores[node_idx][par_state];
            for (int j=0; j<4; j++) {
                if (scores[node_idx][j] < min_s) {
                    min_s = scores[node_idx][j];
                    state = j;
                }
            }
            if (state != par_state) {
                if (scores[node_idx][input.ref_nuc] == min_s) {
                    state = input.ref_nuc;
                }
            }
            states[node_idx] = state;
            
            if (state != par_state) {
                if (input.node_mutations->find(node) != input.node_mutations->end()) {
                    mutation m;
                    m.chrom = input.chrom;
                    m.position = input.variant_pos;
                    m.ref_nuc = input.ref_nuc;
                    m.par_nuc = par_state;
                    m.mut_nuc.emplace_back(state);

                    data_lock.lock();
                    (*input.node_mutations)[node].emplace_back(m);
                    data_lock.unlock();
                }
                else {
                    mutation m;
                    m.chrom = input.chrom;
                    m.position = input.variant_pos;
                    m.ref_nuc = input.ref_nuc;
                    m.par_nuc = par_state;
                    m.mut_nuc.emplace_back(state);

                    data_lock.lock();
                    (*input.node_mutations).insert(std::pair<Node*, std::vector<mutation>>(node, std::vector<mutation>()));  
                    (*input.node_mutations)[node].emplace_back(m);
                    data_lock.unlock();
                }
            }
        }

    }

    return 1;
}

void mapper2_body(mapper2_input& input, bool compute_parsimony_scores) {
    //    TIMEIT();

    int set_difference = 0;
    int best_set_difference = *input.best_set_difference;


    std::vector<int> anc_positions;
    std::vector<mutation> ancestral_mutations;

    bool has_unique = false;
    int node_num_mut = 0;
    int num_common_mut = 0;

#if DEBUG == 1
    assert (std::is_sorted((*input.missing_sample_mutations).begin(), (*input.missing_sample_mutations).end(), compare_by_position));
#endif

    if (!input.node->is_root()) {
        if (input.node_mutations->find(input.node) != input.node_mutations->end()) {
            size_t start_index = 0;
#if DEBUG == 1
            assert(std::is_sorted((*input.node_mutations)[input.node].begin(), (*input.node_mutations)[input.node].end(), compare_by_position));
#endif
            for (auto m1: (*input.node_mutations)[input.node]) {
                node_num_mut++;
                auto anc_nuc = m1.mut_nuc[0];
                bool found = false;
                bool found_pos = false;
                for (size_t k = start_index; k < input.missing_sample_mutations->size(); k++) {
                    auto m2 = (*input.missing_sample_mutations)[k];
                    start_index = k;
                    if (m1.position == m2.position) {
                        found_pos = true;
                        if (m2.is_missing) {
                            found = true;
                            num_common_mut++;
                        }
                        else {
                            for (auto nuc: m2.mut_nuc) {
                                if (nuc == anc_nuc) {
                                    mutation m;
                                    m.chrom = m1.chrom;
                                    m.position = m1.position;
                                    m.ref_nuc = m1.ref_nuc;
                                    m.par_nuc = m1.par_nuc;
                                    m.mut_nuc.emplace_back(anc_nuc);

                                    ancestral_mutations.emplace_back(m);
                                    anc_positions.emplace_back(m1.position);
                                    (*input.excess_mutations).emplace_back(m);
                                    if (m2.mut_nuc.size() > 1) {
                                        (*input.imputed_mutations).emplace_back(m);
                                    }
                                    found = true;
                                    num_common_mut++;
                                    break;
                                }
                            }
                        }
                    }
                    if (m1.position <= m2.position) {
                        break;
                    }
                }
                if (!found) {
                    if (!found_pos && (anc_nuc == m1.ref_nuc)) {
                        mutation m;
                        m.position = m1.position;
                        m.ref_nuc = m1.ref_nuc;
                        m.par_nuc = m1.par_nuc;
                        m.mut_nuc.emplace_back(anc_nuc);

                        ancestral_mutations.emplace_back(m);
                        anc_positions.emplace_back(m1.position);
                        (*input.excess_mutations).emplace_back(m);
                        num_common_mut++;
                    }
                    else {
                        has_unique = true;
                    }
                }
            }
        }
    }
    else {
        if (input.node_mutations->find(input.node) != input.node_mutations->end()) {
            for (auto m: (*input.node_mutations)[input.node]) {
                ancestral_mutations.emplace_back(m);
                anc_positions.emplace_back(m.position);
            }
        }
    }

    for (auto n: input.T->rsearch(input.node->identifier)) {
        if (input.node_mutations->find(n) != input.node_mutations->end()) {
            for (auto m: (*input.node_mutations)[n]) {
                if (std::find(anc_positions.begin(), anc_positions.end(), m.position) == anc_positions.end()) {
                    ancestral_mutations.emplace_back(m);
                    anc_positions.emplace_back(m.position);
                }
            }
        }
    }

    std::sort(ancestral_mutations.begin(), ancestral_mutations.end(), compare_by_position);

    for (auto m1: (*input.missing_sample_mutations)) {
        if (m1.is_missing) {
            continue;
        }
        size_t start_index = 0;
        bool found_pos = false;
        bool found = false;
        bool has_ref = false;
        auto anc_nuc = m1.ref_nuc;
        for (auto nuc: m1.mut_nuc) {
            if (nuc == m1.ref_nuc) {
                has_ref = true;
            }
        }
        for (size_t k = start_index; k < ancestral_mutations.size(); k++) {
            auto m2 = ancestral_mutations[k];
            start_index = k;
            if (m1.position == m2.position) {
                found_pos = true;
                anc_nuc = m2.mut_nuc[0];
                for (auto nuc: m1.mut_nuc) {
                    if (nuc == anc_nuc) {
                        found = true;
                    }
                }
                break;
            }
        }
        if (found) {
            if (m1.mut_nuc.size() > 1) {
                mutation m;
                m.chrom = m1.chrom;
                m.position = m1.position;
                m.ref_nuc = m1.ref_nuc;
                m.par_nuc = anc_nuc;
                m.mut_nuc.emplace_back(anc_nuc);
                input.imputed_mutations->emplace_back(m);
            }
        }
        else if (!found_pos && has_ref) {
            if (m1.mut_nuc.size() > 1) {
                mutation m;
                m.chrom = m1.chrom;
                m.position = m1.position;
                m.ref_nuc = m1.ref_nuc;
                m.par_nuc = anc_nuc;
                m.mut_nuc.emplace_back(m1.ref_nuc);
                input.imputed_mutations->emplace_back(m);
            }
        }
        else {
            set_difference += 1;
            if (!compute_parsimony_scores && (set_difference > best_set_difference)) {
                return;
            }
            mutation m;
            m.chrom = m1.chrom;
            m.position = m1.position;
            m.ref_nuc = m1.ref_nuc;
            m.par_nuc = anc_nuc;
            if (has_ref) {
                m.mut_nuc.emplace_back(m1.ref_nuc);
            }
            else {
                m.mut_nuc.emplace_back(m1.mut_nuc[0]);
            }
            input.excess_mutations->emplace_back(m);
            if (m1.mut_nuc.size() > 1) {
                input.imputed_mutations->emplace_back(m);
            }
        }
    }

    for (auto m1: ancestral_mutations) {
        size_t start_index = 0;
        bool found = false;
        bool found_pos = false;
        auto anc_nuc = m1.mut_nuc[0];
        for (size_t k = start_index; k < input.missing_sample_mutations->size(); k++) {
            auto m2 = (*input.missing_sample_mutations)[k];
            start_index = k;
            if (m1.position == m2.position) {
                found_pos = true;
                if (m2.is_missing) {
                    found = true;
                    break;
                }
                for (auto nuc: m2.mut_nuc) {
                    if (nuc == anc_nuc) {
                        found = true;
                    }
                }
            }
        }
        if (found) {
        }
        else if (!found_pos && (anc_nuc == m1.ref_nuc)) {
        }
        else {
            set_difference += 1;
            if (!compute_parsimony_scores && (set_difference > best_set_difference)) {
                return;
            }
            mutation m;
            m.chrom = m1.chrom;
            m.position = m1.position;
            m.ref_nuc = m1.ref_nuc;
            m.par_nuc = anc_nuc;
            m.mut_nuc.emplace_back(m1.ref_nuc);
            (*input.excess_mutations).emplace_back(m);
        }
    }

    if (compute_parsimony_scores) {
        *input.set_difference = set_difference;
    }

    // if sibling of internal node or leaf, ensure it is not equivalent to placing under parent
    // if child of internal node, ensure all internal node mutations are present in the sample
    if ((has_unique && !input.node->is_leaf() && (num_common_mut > 0) && (node_num_mut != num_common_mut)) || \
            (input.node->is_leaf() && (num_common_mut > 0)) || (!has_unique && !input.node->is_leaf() && (node_num_mut == num_common_mut))) { 
        data_lock.lock();
        if (set_difference > *input.best_set_difference) {
            data_lock.unlock();
            return;
        }
        size_t num_leaves = input.T->get_num_leaves(input.node);
        if (set_difference < *input.best_set_difference) {
            *input.best_set_difference = set_difference;
            *input.best_node = input.node;
            *input.best_node_num_leaves = num_leaves; 
            *input.best_j = input.j;
            *input.num_best = 1;
            *input.has_unique = has_unique;
#if DEBUG == 1
            (*input.node_has_unique)[input.j] = has_unique;
            input.best_j_vec->clear();
            input.best_j_vec->emplace_back(input.j);
#endif
        }
        else if (set_difference == *input.best_set_difference) {
            //bool is_best_node_ancestor = (*input.best_j > input.j) ? input.T->is_ancestor((*input.best_node)->identifier, input.node->identifier) : false;
            //bool is_best_node_descendant = (*input.best_j < input.j) ? input.T->is_ancestor(input.node->identifier, (*input.best_node)->identifier) : false;
            bool is_best_node_ancestor = (input.node->parent == (*input.best_node)); 
            bool is_best_node_descendant = ((*input.best_node)->parent == input.node); 
            
            if ((is_best_node_ancestor && (2*num_leaves > *input.best_node_num_leaves)) ||
                    (is_best_node_descendant && (num_leaves >= 2*(*input.best_node_num_leaves))) ||
                    (!is_best_node_ancestor && !is_best_node_descendant && (num_leaves > *input.best_node_num_leaves)) ||
                    (!is_best_node_ancestor && !is_best_node_descendant && (num_leaves == *input.best_node_num_leaves) && (*input.best_j < input.j))) 
            {
                *input.best_set_difference = set_difference;
                *input.best_node = input.node;
                *input.best_node_num_leaves = num_leaves; 
                *input.best_j = input.j;
                *input.has_unique = has_unique;
            }
            *input.num_best += 1;
#if DEBUG == 1
            (*input.node_has_unique)[input.j] = has_unique;
            input.best_j_vec->emplace_back(input.j);
#endif
        }
        data_lock.unlock();
    }
    else if (compute_parsimony_scores) {
        // Add 1 to the parsimony score for this node 
        // as its current best placement is equivalent  
        // to placing at parent/child
        *input.set_difference = set_difference + 1;
    }
}
