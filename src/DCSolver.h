// Copyright (c) 2020, RTE (https://www.rte-france.com)
// See AUTHORS.txt
// This Source Code Form is subject to the terms of the Mozilla Public License, version 2.0.
// If a copy of the Mozilla Public License, version 2.0 was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0
// This file is part of LightSim2grid, LightSim2grid implements a c++ backend targeting the Grid2Op platform.

#ifndef DCSOLVER_H
#define DCSOLVER_H

#include "BaseSolver.h"
// TODO make err_ more explicit: use an enum
class DCSolver: public BaseSolver
{
    public:
        DCSolver():BaseSolver(){};

        ~DCSolver(){}

        virtual
        bool compute_pf(const Eigen::SparseMatrix<cdouble> & Ybus,
                        Eigen::VectorXcd & V,
                        const Eigen::VectorXcd & Sbus,
                        const Eigen::VectorXi & pv,
                        const Eigen::VectorXi & pq,
                        int max_iter,
                        double tol
                        );

    private:
        // no copy allowed
        DCSolver( const BaseSolver & ) ;
        DCSolver & operator=( const BaseSolver & ) ;

};

#endif // DCSOLVER_H
