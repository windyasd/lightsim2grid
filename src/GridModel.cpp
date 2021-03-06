// Copyright (c) 2020, RTE (https://www.rte-france.com)
// See AUTHORS.txt
// This Source Code Form is subject to the terms of the Mozilla Public License, version 2.0.
// If a copy of the Mozilla Public License, version 2.0 was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0
// This file is part of LightSim2grid, LightSim2grid implements a c++ backend targeting the Grid2Op platform.

#include "GridModel.h"

GridModel::GridModel(const GridModel & other)
{
    reset();

    // assign the right solver
    _solver.change_solver(other._solver.get_type());
    compute_results_ = other.compute_results_;

    // copy the powersystem representation
    // 1. bus
    bus_vn_kv_.array() = other.bus_vn_kv_;
    bus_status_ = other.bus_status_;

    // 2. powerline
    powerlines_ = other.powerlines_;

    // 3. shunt
    shunts_ = other.shunts_;

    // 4. transformers
    // have the r, x, h and ratio
    // ratio is computed from the tap, so maybe store tap num and tap_step_pct
    trafos_ = other.trafos_;

    // 5. generators
    generators_ = other.generators_;

    // 6. loads
    loads_ = other.loads_;

    // 7. slack bus
    gen_slackbus_ = other.gen_slackbus_;
    slack_bus_id_ = other.slack_bus_id_;

    // copy the attributes specific grid2op (speed optimization)
    n_sub_ = other.n_sub_;
    load_pos_topo_vect_ = other.load_pos_topo_vect_;
    gen_pos_topo_vect_ = other.gen_pos_topo_vect_;
    line_or_pos_topo_vect_ = other.line_or_pos_topo_vect_;
    line_ex_pos_topo_vect_ = other.line_ex_pos_topo_vect_;
    trafo_hv_pos_topo_vect_ = other.trafo_hv_pos_topo_vect_;
    trafo_lv_pos_topo_vect_ = other.trafo_lv_pos_topo_vect_;

    load_to_subid_ = other.load_to_subid_;
    gen_to_subid_ = other.gen_to_subid_;
    line_or_to_subid_ = other.line_or_to_subid_;
    line_ex_to_subid_ = other.line_ex_to_subid_;
    trafo_hv_to_subid_ = other.trafo_hv_to_subid_;
    trafo_lv_to_subid_ = other.trafo_lv_to_subid_;
}

//pickle
GridModel::StateRes GridModel::get_state() const
{
    std::vector<double> bus_vn_kv(bus_vn_kv_.begin(), bus_vn_kv_.end());
    auto res_line = powerlines_.get_state();
    auto res_shunt = shunts_.get_state();
    auto res_trafo = trafos_.get_state();
    auto res_gen = generators_.get_state();
    auto res_load = loads_.get_state();

    GridModel::StateRes res(bus_vn_kv,
                            bus_status_,
                            res_line,
                            res_shunt,
                            res_trafo,
                            res_gen,
                            res_load,
                            gen_slackbus_
                            );
    return res;
};

void GridModel::set_state(GridModel::StateRes & my_state)
{
    // after loading back, the instance need to be reset anyway
    // TODO see if it's worth the trouble NOT to do it
    reset();
    need_reset_ = true;
    compute_results_ = true;

    // extract data from the state
    std::vector<double> & bus_vn_kv = std::get<0>(my_state);
    std::vector<bool> & bus_status = std::get<1>(my_state);

    // powerlines
    DataLine::StateRes & state_lines = std::get<2>(my_state);
    // shunts
    DataShunt::StateRes & state_shunts = std::get<3>(my_state);
    // trafos
    DataTrafo::StateRes & state_trafos = std::get<4>(my_state);
    // generators
    DataGen::StateRes & state_gens = std::get<5>(my_state);
    // loads
    DataLoad::StateRes & state_loads = std::get<6>(my_state);
    int gen_slackbus = std::get<7>(my_state);

    // assign it to this instance

    // buses
    // 1. bus_vn_kv_
    bus_vn_kv_ = Eigen::VectorXd::Map(&bus_vn_kv[0], bus_vn_kv.size());
    // 2. bus status
    bus_status_ = bus_status;

    // elements
    // 1. powerlines
    powerlines_.set_state(state_lines);
    // 2. shunts
    shunts_.set_state(state_shunts);
    // 3. trafos
    trafos_.set_state(state_trafos);
    // 4. gen
    generators_.set_state(state_gens);
    // 5. loads
    loads_.set_state(state_loads);

    // other stuff
    gen_slackbus_ = gen_slackbus;

};

//init
void GridModel::init_bus(const Eigen::VectorXd & bus_vn_kv, int nb_line, int nb_trafo){
    /**
    initialize the bus_vn_kv_ member
    and
    initialize the Ybus_ matrix at the proper shape
    **/
    int nb_bus = bus_vn_kv.size();
    bus_vn_kv_ = bus_vn_kv;  // base_kv

    bus_status_ = std::vector<bool>(nb_bus, true); // by default everything is connected
}

void GridModel::reset()
{
    Ybus_ = Eigen::SparseMatrix<cdouble>();
    Sbus_ = Eigen::VectorXcd();
    id_me_to_solver_ = std::vector<int>();
    id_solver_to_me_ = std::vector<int>();
    slack_bus_id_solver_ = -1;
    bus_pv_ = Eigen::VectorXi();
    bus_pq_ = Eigen::VectorXi();
    need_reset_ = true;

    // reset the solvers
    _solver.reset();
}

Eigen::VectorXcd GridModel::ac_pf(const Eigen::VectorXcd & Vinit,
                                  int max_iter,
                                  double tol)
{
    int nb_bus = bus_vn_kv_.size();
    if(Vinit.size() != nb_bus){
        std::cout << "Vinit.size() " << Vinit.size() << " nb_bus: " << nb_bus << std::endl;
        throw std::runtime_error("Size of the Vinit should be the same as the total number of buses (both connected and disconnected). (fyi: Components of Vinit corresponding to deactivated bus will be ignored anyway, so you can put whatever you want there).");
    }
    bool conv = false;
    Eigen::VectorXcd res = Eigen::VectorXcd();

    // pre process the data to define a proper jacobian matrix, the proper voltage vector etc.
    Eigen::VectorXcd V = pre_process_solver(Vinit, true);

    // start the solver
    conv = _solver.compute_pf(Ybus_, V, Sbus_, bus_pv_, bus_pq_, max_iter, tol);

    // store results
    process_results(conv, res, Vinit);

    // return the vector of complex voltage at each bus
    return res;
};

Eigen::VectorXcd GridModel::pre_process_solver(const Eigen::VectorXcd & Vinit, bool is_ac)
{
    // TODO get rid of the "is_ac" argument: this info is available in the _solver already

    // if(need_reset_){ // TODO optimization when it's not mandatory to start from scratch
    reset();
    slack_bus_id_ = generators_.get_slack_bus_id(gen_slackbus_);
    init_Ybus(Ybus_, Sbus_, id_me_to_solver_, id_solver_to_me_, slack_bus_id_solver_);
    fillYbus(Ybus_, is_ac, id_me_to_solver_);
    fillpv_pq(id_me_to_solver_);
    generators_.init_q_vector(bus_vn_kv_.size());
    // }
    fillSbus_me(Sbus_, is_ac, id_me_to_solver_, slack_bus_id_solver_);

    int nb_bus_solver = id_solver_to_me_.size();
    Eigen::VectorXcd V = Eigen::VectorXcd::Constant(nb_bus_solver, 1.04);
    for(int bus_solver_id = 0; bus_solver_id < nb_bus_solver; ++bus_solver_id){
        int bus_me_id = id_solver_to_me_[bus_solver_id];  //POSSIBLE SEGFAULT
        cdouble tmp = Vinit(bus_me_id);
        V(bus_solver_id) = tmp;
        // TODO save this V somewhere
    }
    generators_.set_vm(V, id_me_to_solver_);
    return V;
}
void GridModel::process_results(bool conv, Eigen::VectorXcd & res, const Eigen::VectorXcd & Vinit)
{
    if (conv){
        if(compute_results_){
            // compute the results of the flows, P,Q,V of loads etc.
            compute_results();
        }
        need_reset_ = false;
        Eigen::VectorXcd res_tmp = _solver.get_V();
        // convert back the results to "big" vector
        res = Eigen::VectorXcd::Constant(Vinit.size(), 0.);
        int nb_bus = bus_vn_kv_.size();
        for (int bus_id_me=0; bus_id_me < nb_bus; ++bus_id_me){
            if(!bus_status_[bus_id_me]) continue;  // nothing is done if the bus is connected
            int bus_id_solver = id_me_to_solver_[bus_id_me];
            if(bus_id_solver == _deactivated_bus_id){
                //TODO improve error message with the gen_id
                throw std::runtime_error("One bus is connected in GridModel and disconnected in Solver");
            }
            res(bus_id_me) = res_tmp(bus_id_solver);
        }
    } else {
        //powerflow diverge
        reset_results();
        need_reset_ = true;  // in this case, the powerflow diverge, so i need to recompute Ybus next time
    }
}
void GridModel::init_Ybus(Eigen::SparseMatrix<cdouble> & Ybus,
                          Eigen::VectorXcd & Sbus,
                          std::vector<int>& id_me_to_solver,
                          std::vector<int>& id_solver_to_me,
                          int & slack_bus_id_solver){
    //TODO get disconnected bus !!! (and have some conversion for it)
    //1. init the conversion bus
    int nb_bus_init = bus_vn_kv_.size();
    id_me_to_solver = std::vector<int>(nb_bus_init, _deactivated_bus_id);  // by default, if a bus is disconnected, then it has a -1 there
    id_solver_to_me = std::vector<int>();
    id_solver_to_me.reserve(nb_bus_init);
    int bus_id_solver=0;
    for(int bus_id_me=0; bus_id_me < nb_bus_init; ++bus_id_me){
        if(bus_status_[bus_id_me]){
            // bus is connected
            id_solver_to_me.push_back(bus_id_me);
            id_me_to_solver[bus_id_me] = bus_id_solver;
            ++bus_id_solver;
        }
    }
    int nb_bus = id_solver_to_me.size();

    Ybus = Eigen::SparseMatrix<cdouble>(nb_bus, nb_bus);
    Ybus.reserve(nb_bus + 2*powerlines_.nb() + 2*trafos_.nb());

    Sbus = Eigen::VectorXcd::Constant(nb_bus, 0.);
    slack_bus_id_solver = id_me_to_solver[slack_bus_id_];
    if(slack_bus_id_solver == _deactivated_bus_id){
        //TODO improve error message with the gen_id
        throw std::runtime_error("The slack bus is disconnected.");
    }
}

void GridModel::fillYbus(Eigen::SparseMatrix<cdouble> & res, bool ac, const std::vector<int>& id_me_to_solver){
    /**
    Supposes that the powerlines, shunt and transformers are initialized.
    And it fills the Ybus matrix.
    **/

    // init the Ybus matrix
    std::vector<Eigen::Triplet<cdouble> > tripletList;
    tripletList.reserve(bus_vn_kv_.size() + 4*powerlines_.nb() + 4*trafos_.nb() + shunts_.nb());
    powerlines_.fillYbus(tripletList, ac, id_me_to_solver);
    shunts_.fillYbus(tripletList, ac, id_me_to_solver);
    trafos_.fillYbus(tripletList, ac, id_me_to_solver);
    loads_.fillYbus(tripletList, ac, id_me_to_solver);
    generators_.fillYbus(tripletList, ac, id_me_to_solver);
    res.setFromTriplets(tripletList.begin(), tripletList.end());
    res.makeCompressed();
}

void GridModel::fillSbus_me(Eigen::VectorXcd & res, bool ac, const std::vector<int>& id_me_to_solver, int slack_bus_id_solver)
{
    // init the Sbus vector
    powerlines_.fillSbus(res, ac, id_me_to_solver);
    shunts_.fillSbus(res, ac, id_me_to_solver);
    trafos_.fillSbus(res, ac, id_me_to_solver);
    loads_.fillSbus(res, ac, id_me_to_solver);
    generators_.fillSbus(res, ac, id_me_to_solver);

    // handle slack bus
    double sum_active = res.sum().real();
    res.coeffRef(slack_bus_id_solver) -= sum_active;
}

void GridModel::fillpv_pq(const std::vector<int>& id_me_to_solver)
{
    // init pq and pv vector
    // TODO remove the order here..., i could be faster in this piece of code (looping once through the buses)
    int nb_bus = id_solver_to_me_.size();  // number of bus in the solver!
    std::vector<int> bus_pq;
    std::vector<int> bus_pv;
    std::vector<bool> has_bus_been_added(nb_bus, false);

    bus_pv_ = Eigen::VectorXi();
    bus_pq_ = Eigen::VectorXi();
    powerlines_.fillpv(bus_pv, has_bus_been_added, slack_bus_id_solver_, id_me_to_solver);
    shunts_.fillpv(bus_pv, has_bus_been_added, slack_bus_id_solver_, id_me_to_solver);
    trafos_.fillpv(bus_pv, has_bus_been_added, slack_bus_id_solver_, id_me_to_solver);
    loads_.fillpv(bus_pv, has_bus_been_added, slack_bus_id_solver_, id_me_to_solver);
    generators_.fillpv(bus_pv, has_bus_been_added, slack_bus_id_solver_, id_me_to_solver);

    for(int bus_id = 0; bus_id< nb_bus; ++bus_id){
        if(bus_id == slack_bus_id_solver_) continue;  // slack bus is not PQ either
        if(has_bus_been_added[bus_id]) continue; // a pv bus cannot be PQ
        bus_pq.push_back(bus_id);
        has_bus_been_added[bus_id] = true;  // don't add it a second time
    }
    bus_pv_ = Eigen::Map<Eigen::VectorXi, Eigen::Unaligned>(bus_pv.data(), bus_pv.size());
    bus_pq_ = Eigen::Map<Eigen::VectorXi, Eigen::Unaligned>(bus_pq.data(), bus_pq.size());
}
void GridModel::compute_results(){
    // TODO "deactivate" the Q value for DC

    // retrieve results from powerflow
    const auto & Va = _solver.get_Va();
    const auto & Vm = _solver.get_Vm();
    const auto & V = _solver.get_V();
    // for powerlines
    powerlines_.compute_results(Va, Vm, V, id_me_to_solver_, bus_vn_kv_);
    // for trafo
    trafos_.compute_results(Va, Vm, V, id_me_to_solver_, bus_vn_kv_);
    // for loads
    loads_.compute_results(Va, Vm, V, id_me_to_solver_, bus_vn_kv_);
    // for shunts
    shunts_.compute_results(Va, Vm, V, id_me_to_solver_, bus_vn_kv_);
    // for prods
    generators_.compute_results(Va, Vm, V, id_me_to_solver_, bus_vn_kv_);

    //handle_slack_bus
    double p_slack = powerlines_.get_p_slack(slack_bus_id_);
    p_slack += trafos_.get_p_slack(slack_bus_id_);
    p_slack += loads_.get_p_slack(slack_bus_id_);
    p_slack += shunts_.get_p_slack(slack_bus_id_);
    generators_.set_p_slack(gen_slackbus_, p_slack);

    // handle gen_q now
    std::vector<double> q_by_bus = std::vector<double>(bus_vn_kv_.size(), 0.);
    powerlines_.get_q(q_by_bus);
    trafos_.get_q(q_by_bus);
    loads_.get_q(q_by_bus);
    shunts_.get_q(q_by_bus);

    generators_.set_q(q_by_bus);
}

void GridModel::reset_results(){
    powerlines_.reset_results();
    shunts_.reset_results();
    trafos_.reset_results();
    loads_.reset_results();
    generators_.reset_results();
}

Eigen::VectorXcd GridModel::dc_pf_old(const Eigen::VectorXcd & Vinit,
                                      int max_iter,  // not used for DC
                                      double tol  // not used for DC
                                      )
{
    // TODO refactor that with ac pf, this is mostly done, but only mostly...
    int nb_bus = bus_vn_kv_.size();
    if(Vinit.size() != nb_bus){
        std::cout << "Vinit.size() " << Vinit.size() << " nb_bus: " << nb_bus << std::endl;
        throw std::runtime_error("Size of the Vinit should be the same as the total number of buses (both conencted and disconnected). (fyi: Components of Vinit corresponding to deactivated bus will be ignored anyway, so you can put whatever you want there.)");
    }
    Eigen::SparseMatrix<cdouble> dcYbus_tmp;
    Eigen::VectorXcd Sbus_tmp;
    std::vector<int> id_me_to_solver;
    std::vector<int> id_solver_to_me;
    int slack_bus_id_solver;

    //if(need_reset_){
    slack_bus_id_ = generators_.get_slack_bus_id(gen_slackbus_);
    init_Ybus(dcYbus_tmp, Sbus_tmp, id_me_to_solver, id_solver_to_me, slack_bus_id_solver);
    fillYbus(dcYbus_tmp, false, id_me_to_solver);
    // fillpv_pq(id_me_to_solver);
    //}
    fillSbus_me(Sbus_tmp, false, id_me_to_solver, slack_bus_id_solver);


    // extract only connected bus from Vinit
    int nb_bus_solver = id_solver_to_me.size();

    // DC SOLVER STARTS HERE
    // TODO all this should rather be one in a "dc solver" instead of here
    // remove the slack bus

    // remove the slack bus from Ybus
    // TODO see if "prune" might work here https://eigen.tuxfamily.org/dox/classEigen_1_1SparseMatrix.html#title29
    dcYbus_tmp.makeCompressed();
    Eigen::SparseMatrix<double> dcYbus = Eigen::SparseMatrix<double>(nb_bus_solver - 1, nb_bus_solver - 1);
    std::vector<Eigen::Triplet<double> > tripletList;
    tripletList.reserve(dcYbus_tmp.nonZeros());
    for (int k=0; k < nb_bus_solver; ++k){
        if(k == slack_bus_id_solver) continue;  // I don't add anything to the slack bus
        for (Eigen::SparseMatrix<cdouble>::InnerIterator it(dcYbus_tmp, k); it; ++it)
        {
            int row_res = it.row();
            if(row_res == slack_bus_id_solver) continue;
            row_res = row_res > slack_bus_id_solver ? row_res - 1 : row_res;
            int col_res = it.col();
            col_res = col_res > slack_bus_id_solver ? col_res - 1 : col_res;
            tripletList.push_back(Eigen::Triplet<double> (row_res, col_res, std::real(it.value())));
        }
    }
    dcYbus.setFromTriplets(tripletList.begin(), tripletList.end());
    dcYbus.makeCompressed();

    // initialize the solver
    Eigen::SparseLU<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int> >   solver;
    solver.analyzePattern(dcYbus);
    solver.factorize(dcYbus);
    if(solver.info() != Eigen::Success) {
        // matrix is not connected
        return Eigen::VectorXcd();
    }

    // remove the slack bus from Sbus
    Eigen::VectorXd Sbus = Eigen::VectorXd::Constant(nb_bus_solver - 1, 0.);
    for (int k=0; k < nb_bus_solver; ++k){
        if(k == slack_bus_id_solver) continue;  // I don't add anything to the slack bus
        int col_res = k;
        col_res = col_res > slack_bus_id_solver ? col_res - 1 : col_res;
        Sbus(col_res) = std::real(Sbus_tmp(k));
    }

    // solve for theta: Sbus = dcY . theta
    Eigen::VectorXd Va_dc = solver.solve(Sbus);
    if(solver.info() != Eigen::Success) {
        // solving failed, this should not happen in dc ...
        return Eigen::VectorXcd();
    }

    // retrieve back the results in the proper shape
    int nb_bus_me = bus_vn_kv_.size();
    int bus_id_solver;
    Eigen::VectorXd Va = Eigen::VectorXd::Constant(nb_bus_me, 0.);
    // fill Va from dc approx
    for (int bus_id_me=0; bus_id_me < nb_bus_me; ++bus_id_me){
        if(bus_id_me == slack_bus_id_) continue;  // slack bus is handled elsewhere
        if(!bus_status_[bus_id_me]) continue;  // nothing is done if the bus is not connected

        bus_id_solver = id_me_to_solver[bus_id_me];
        if(bus_id_solver == _deactivated_bus_id){
            //TODO improve error message with the gen_id
            throw std::runtime_error("One bus is both connected and disconnected");
        }
        bus_id_solver = bus_id_solver > slack_bus_id_solver ? bus_id_solver - 1 : bus_id_solver;
        Va(bus_id_me) = Va_dc(bus_id_solver);
    }
    Va.array() +=  std::arg(Vinit(slack_bus_id_));

    // fill Vm either Vinit if pq or Vm if pv (TODO)
    Eigen::VectorXd Vm;
    if(false){
        Vm = Vinit.array().abs();  // fill Vm = Vinit for all
        // put Vm = 0. for disconnected bus
        for (int bus_id_me=0; bus_id_me < nb_bus_me; ++bus_id_me){
            if(bus_status_[bus_id_me]) continue;  // nothing is done if the bus is connected
            Vm(bus_id_me) = 0.;
        }
        // put Vm = Vm of turned on gen
        // generators_.get_vm_for_dc(Vm);
        // assign vm of the slack bus
        Vm(slack_bus_id_) =  std::abs(Vinit(slack_bus_id_));
    }
    else{
        Vm = Eigen::VectorXd::Constant(Vinit.size(), 1.0);
        for (int bus_id_me=0; bus_id_me < nb_bus_me; ++bus_id_me){
            if(bus_status_[bus_id_me]) continue;  // nothing is done if the bus is connected
            Vm(bus_id_me) = 0.;
        }
        generators_.get_vm_for_dc(Vm);
    }
    //END of the SOLVER PART

    //TODO handle Vm = Vm (gen) for connected generators
    return Vm.array() * (Va.array().cos().cast<cdouble>() + my_i * Va.array().sin().cast<cdouble>());
}

Eigen::VectorXcd GridModel::dc_pf(const Eigen::VectorXcd & Vinit,
                                  int max_iter,  // not used for DC
                                  double tol  // not used for DC
                                  )
{
    int nb_bus = bus_vn_kv_.size();
    if(Vinit.size() != nb_bus){
        std::cout << "Vinit.size() " << Vinit.size() << " nb_bus: " << nb_bus << std::endl;
        throw std::runtime_error("Size of the Vinit should be the same as the total number of buses (both connected and disconnected). (fyi: Components of Vinit corresponding to deactivated bus will be ignored anyway, so you can put whatever you want there).");
    }
    SolverType solver_type = _solver.get_type();
    _solver.change_solver(SolverType::DC);

    bool conv = false;
    Eigen::VectorXcd res = Eigen::VectorXcd();

    // pre process the data to define a proper jacobian matrix, the proper voltage vector etc.
    Eigen::VectorXcd V = pre_process_solver(Vinit, false);

    // start the solver
    conv = _solver.compute_pf(Ybus_, V, Sbus_, bus_pv_, bus_pq_, max_iter, tol);

    // store results
    process_results(conv, res, Vinit);

    // put back the solver to its original state
    // TODO add a better handling of this!
    _solver.change_solver(solver_type);

    // return the vector of complex voltage at each bus
    return res;
}

/**
Retrieve the number of connected buses
**/
int GridModel::nb_bus() const
{
    int res = 0;
    for(const auto & el : bus_status_)
    {
        if(el) ++res;
    }
    return res;
}

void GridModel::add_gen_slackbus(int gen_id){
    if(gen_id < 0) throw std::runtime_error("Slack bus should be an id of a generator, thus positive");
    if(gen_id > generators_.nb()) throw std::runtime_error("Slack bus should be an id of a generator, your id is to high.");
    gen_slackbus_ = gen_id;
}

/** GRID2OP SPECIFIC REPRESENTATION **/
void GridModel::update_bus_status(int nb_bus_before,
                                  Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, 2, Eigen::RowMajor> > active_bus)
{
    for(int bus_id = 0; bus_id < active_bus.rows(); ++bus_id)
    {
        if(active_bus(bus_id, 0)){
            reactivate_bus(bus_id);
        }else{
            deactivate_bus(bus_id);
        }
        if(active_bus(bus_id, 1)){
            reactivate_bus(bus_id + nb_bus_before);
        }else{
            deactivate_bus(bus_id + nb_bus_before);
        }
    }
}

void GridModel::update_gens_p(Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, Eigen::RowMajor> > has_changed,
                              Eigen::Ref<Eigen::Array<float, Eigen::Dynamic, Eigen::RowMajor> > new_values)
{
    update_continuous_values(has_changed, new_values, &GridModel::change_p_gen);
}
void GridModel::update_gens_v(Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, Eigen::RowMajor> > has_changed,
                              Eigen::Ref<Eigen::Array<float, Eigen::Dynamic, Eigen::RowMajor> > new_values)
{
    update_continuous_values(has_changed, new_values, &GridModel::change_v_gen);
}
void GridModel::update_loads_p(Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, Eigen::RowMajor> > has_changed,
                              Eigen::Ref<Eigen::Array<float, Eigen::Dynamic, Eigen::RowMajor> > new_values)
{
    update_continuous_values(has_changed, new_values, &GridModel::change_p_load);
}
void GridModel::update_loads_q(Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, Eigen::RowMajor> > has_changed,
                              Eigen::Ref<Eigen::Array<float, Eigen::Dynamic, Eigen::RowMajor> > new_values)
{
    update_continuous_values(has_changed, new_values, &GridModel::change_q_load);
}
void GridModel::update_topo(Eigen::Ref<Eigen::Array<bool, Eigen::Dynamic, Eigen::RowMajor> > has_changed,
                            Eigen::Ref<Eigen::Array<int,  Eigen::Dynamic, Eigen::RowMajor> > new_values)
{
    update_topo_generic(has_changed, new_values,
                        load_pos_topo_vect_, load_to_subid_,
                        &GridModel::reactivate_load,
                        &GridModel::change_bus_load,
                        &GridModel::deactivate_load
                        );
    update_topo_generic(has_changed, new_values,
                        gen_pos_topo_vect_, gen_to_subid_,
                        &GridModel::reactivate_gen,
                        &GridModel::change_bus_gen,
                        &GridModel::deactivate_gen
                        );
    // NB we suppose that if a powerline is disconnected, then both its ends are
    // and same for trafo, obviously
    update_topo_generic(has_changed, new_values,
                        line_or_pos_topo_vect_, line_or_to_subid_,
                        &GridModel::reactivate_powerline,
                        &GridModel::change_bus_powerline_or,
                        &GridModel::deactivate_powerline
                        );
    update_topo_generic(has_changed, new_values,
                        line_ex_pos_topo_vect_, line_ex_to_subid_,
                        &GridModel::reactivate_powerline,
                        &GridModel::change_bus_powerline_ex,
                        &GridModel::deactivate_powerline
                        );
    update_topo_generic(has_changed, new_values,
                        trafo_hv_pos_topo_vect_, trafo_hv_to_subid_,
                        &GridModel::reactivate_trafo,
                        &GridModel::change_bus_trafo_hv,
                        &GridModel::deactivate_trafo
                        );
    update_topo_generic(has_changed, new_values,
                        trafo_lv_pos_topo_vect_, trafo_lv_to_subid_,
                        &GridModel::reactivate_trafo,
                        &GridModel::change_bus_trafo_lv,
                        &GridModel::deactivate_trafo
                        );
}
