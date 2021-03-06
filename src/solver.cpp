#include <vector>
#include <gmsh.h>
#include <utils.h>
#include <iostream>
#include <chrono>
#include <omp.h>
#include <sstream>

#include "configParser.h"
#include "Mesh.h"

namespace solver {

    /**
     * Common variables to all solver
     */
    int elNumNodes;
    int numNodes;
    std::vector<std::string> g_names;
    std::vector<int> elTags;
    std::vector<double> elFlux;
    std::vector<double> elStiffvector;
    std::vector<std::vector<std::vector<double>>> Flux;

    /**
     * Perform a numerical step: u[t+1] = dt*M^-1*(S[u[t]]-F[u[t]]) + beta*u[t]
     * for all elements in mesh object.
     *
     * @param mesh Mesh object
     * @param config Configuration file
     * @param u Nodal solution vector
     * @param Flux Nodal physical Flux
     * @param beta double coefficient
     */
    void numStep(Mesh &mesh, Config config, std::vector<std::vector<double>> &u,
                 std::vector<std::vector<std::vector<double>>> &Flux, double beta){

        for(int eq=0; eq<4; ++eq){
            mesh.precomputeFlux(u[eq], Flux[eq], eq);

            #pragma omp parallel for schedule(static) firstprivate(elFlux, elStiffvector) num_threads(config.numThreads)
            for(int el=0; el<mesh.getElNum(); ++el){

                mesh.getElFlux(el, elFlux.data());
                mesh.getElStiffVector(el, Flux[eq], u[eq], elStiffvector.data());
                eigen::minus(elStiffvector.data(), elFlux.data(), elNumNodes);
                eigen::linEq(&mesh.elMassMatrix(el), &elStiffvector[0], &u[eq][el*elNumNodes],
                             config.timeStep, beta, elNumNodes);
            }
        }

    }

    /**
     * Solve using forward explicit scheme. O(h)
     *
     * @param u initial nodal solution vector
     * @param mesh
     * @param config
     */
    void forwardEuler(std::vector<std::vector<double>> &u, Mesh &mesh, Config config) {

        /** Memory allocation */
        elNumNodes = mesh.getElNumNodes();
        numNodes = mesh.getNumNodes();
        elTags = std::vector<int>(&mesh.elTag(0), &mesh.elTag(0)+mesh.getElNum());
        elFlux.resize(elNumNodes);
        elStiffvector.resize(elNumNodes);
        Flux = std::vector<std::vector<std::vector<double>>>(4,
               std::vector<std::vector<double>>(mesh.getNumNodes(),
               std::vector<double>(3)));

        /** Gmsh save init */
        gmsh::model::list(g_names);
        int gp_viewTag = gmsh::view::add("Pressure");
        int gv_viewTag = gmsh::view::add("Velocity");
        int grho_viewTag = gmsh::view::add("Density");
        std::vector<std::vector<double>> g_p(mesh.getElNum(), std::vector<double>(elNumNodes));
        std::vector<std::vector<double>> g_rho(mesh.getElNum(), std::vector<double>(elNumNodes));
        std::vector<std::vector<double>> g_v(mesh.getElNum(), std::vector<double>(3*elNumNodes));

        /** Precomputation */
        mesh.precomputeMassMatrix();

        /** Source */
        std::vector<std::vector<int>> srcIndices;
        for(int i=0;i<config.sources.size();++i){
            std::vector<int> indice;
            for(int n=0; n<mesh.getNumNodes(); n++) {
                std::vector<double> coord, paramCoord;
                gmsh::model::mesh::getNode(mesh.getElNodeTags()[n], coord, paramCoord);
                if(pow(coord[0]-config.sources[i][1], 2) +
                   pow(coord[1]-config.sources[i][2], 2) +
                   pow(coord[2]-config.sources[i][3], 2)<pow(config.sources[i][4],2)){
                    indice.push_back(n);
                }
            }
            srcIndices.push_back(indice);
        }

        /**
         * Main Loop : Time iteration
         */
        auto start = std::chrono::system_clock::now();
        for(double t=config.timeStart, step=0, tDisplay=0; t<=config.timeEnd;
            t+=config.timeStep, tDisplay+=config.timeStep, ++step) {

            /**
             *  Savings and prints
             */
            if(tDisplay>=config.timeRate || step==0){
                tDisplay = 0;

                /** [1] Copy solution to match GMSH format */
                for(int el = 0; el < mesh.getElNum(); ++el) {
                    for(int n = 0; n < mesh.getElNumNodes(); ++n) {
                        int elN = el*elNumNodes+n;
                        g_p[el][n] = u[0][elN];
                        g_rho[el][n] = u[0][elN]/(config.c0*config.c0);
                        g_v[el][3*n+0] = u[1][elN];
                        g_v[el][3*n+1] = u[2][elN];
                        g_v[el][3*n+2] = u[3][elN];
                    }
                }
                gmsh::view::addModelData(gp_viewTag, step, g_names[0], "ElementNodeData", elTags, g_p, t, 1);
                gmsh::view::addModelData(grho_viewTag, step, g_names[0], "ElementNodeData", elTags, g_rho, t, 1);
                gmsh::view::addModelData(gv_viewTag, step, g_names[0], "ElementNodeData", elTags, g_v, t, 3);

                /** [2] Print and compute iteration time */
                auto end = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end - start);
                gmsh::logger::write("[" + std::to_string(t) + "/" + std::to_string(config.timeEnd) + "s] Step number : "
                                    + std::to_string((int) step) + ", Elapsed time: " + std::to_string(elapsed.count()) +"s");
            }

            /**
             * Update Source
             */
            for(int src=0; src<config.sources.size(); ++src) {
                double amp = config.sources[src][5];
                double freq = config.sources[src][6];
                double phase = config.sources[src][7];
                double duration = config.sources[src][8];
                if(t<duration)
                    for(int n=0; n<srcIndices[src].size(); ++n)
                        u[0][srcIndices[src][n]] =  amp*sin(2*M_PI*freq*t+phase);
            }

            /**
             * First Order Euler
             */
            mesh.updateFlux(u, Flux, config.v0, config.c0, config.rho0);
            numStep(mesh, config, u, Flux, 1);

        }

        /** Save to file */
        gmsh::view::write(gp_viewTag, config.saveFile, true);
        gmsh::view::write(grho_viewTag, config.saveFile, true);
        gmsh::view::write(gv_viewTag, config.saveFile, true);
    }


    /**
     * Solve using explicit Runge-Kutta integration method. O(h^4)
     *
     * @param u initial nodal solution vector
     * @param mesh
     * @param config
     */
    void rungeKutta(std::vector<std::vector<double>> &u, Mesh &mesh, Config config) {

        /** Memory allocation */
        elNumNodes = mesh.getElNumNodes();
        numNodes = mesh.getNumNodes();
        elTags = std::vector<int>(&mesh.elTag(0), &mesh.elTag(0)+mesh.getElNum());
        elFlux.resize(elNumNodes);
        elStiffvector.resize(elNumNodes);
        std::vector<std::vector<double>> k1, k2, k3, k4;
        Flux = std::vector<std::vector<std::vector<double>>>(4,
               std::vector<std::vector<double>>(mesh.getNumNodes(),
               std::vector<double>(3)));

        /** Gmsh save init */
        gmsh::model::list(g_names);
        int gp_viewTag = gmsh::view::add("Pressure");
        int gv_viewTag = gmsh::view::add("Velocity");
        int grho_viewTag = gmsh::view::add("Density");
        std::vector<std::vector<double>> g_p(mesh.getElNum(), std::vector<double>(elNumNodes));
        std::vector<std::vector<double>> g_rho(mesh.getElNum(), std::vector<double>(elNumNodes));
        std::vector<std::vector<double>> g_v(mesh.getElNum(), std::vector<double>(3*elNumNodes));

        /** Precomputation (constants over time) */
        mesh.precomputeMassMatrix();

        /** Source */
        std::vector<std::vector<int>> srcIndices;
        for(int i=0;i<config.sources.size();++i){
            std::vector<int> indice;
            for(int n=0; n<mesh.getNumNodes(); n++) {
                std::vector<double> coord, paramCoord;
                gmsh::model::mesh::getNode(mesh.getElNodeTags()[n], coord, paramCoord);
                if(pow(coord[0]-config.sources[i][1], 2) +
                   pow(coord[1]-config.sources[i][2], 2) +
                   pow(coord[2]-config.sources[i][3], 2)<pow(config.sources[i][4],2)){
                    indice.push_back(n);
                }
            }
            srcIndices.push_back(indice);
        }

        /**
         * Main Loop : Time iteration
         */
        auto start = std::chrono::system_clock::now();
        for(double t=config.timeStart, step=0, tDisplay=0; t<=config.timeEnd;
            t+=config.timeStep, tDisplay+=config.timeStep, ++step) {

            /**
             *  Savings and prints
             */
            if(tDisplay>=config.timeRate || step==0){
                tDisplay = 0;

                /** [1] Copy solution to match GMSH format */
                for(int el = 0; el < mesh.getElNum(); ++el) {
                    for(int n = 0; n < mesh.getElNumNodes(); ++n) {
                        int elN = el*elNumNodes+n;
                        g_p[el][n] = u[0][elN];
                        g_rho[el][n] = u[0][elN]/(config.c0*config.c0);
                        g_v[el][3*n+0] = u[1][elN];
                        g_v[el][3*n+1] = u[2][elN];
                        g_v[el][3*n+2] = u[3][elN];
                    }
                }
                gmsh::view::addModelData(gp_viewTag, step, g_names[0], "ElementNodeData", elTags, g_p, t, 1);
                gmsh::view::addModelData(grho_viewTag, step, g_names[0], "ElementNodeData", elTags, g_rho, t, 1);
                gmsh::view::addModelData(gv_viewTag, step, g_names[0], "ElementNodeData", elTags, g_v, t, 3);

                /** [2] Print and compute iteration time */
                auto end = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end - start);
                gmsh::logger::write("[" + std::to_string(t) + "/" + std::to_string(config.timeEnd) + "s] Step number : "
                                    + std::to_string((int) step) + ", Elapsed time: " + std::to_string(elapsed.count()) +"s");
            }

            /** Source */
            for(int src=0; src<config.sources.size(); ++src) {
                double amp = config.sources[src][5];
                double freq = config.sources[src][6];
                double phase = config.sources[src][7];
                double duration = config.sources[src][8];
                if(t<duration)
                    for(int n=0; n<srcIndices[src].size(); ++n)
                        u[0][srcIndices[src][n]] =  amp*sin(2*M_PI*freq*t+phase);
            }

            /**
             * Fourth order Runge-Kutta algorithm
             */
            k1 = k2 = k3 = k4 = u;
            /** [1] Step R-K */
            mesh.updateFlux(k1, Flux, config.v0, config.c0, config.rho0);
            numStep(mesh, config, k1, Flux, 0);
            for(int eq=0; eq<u.size(); ++eq)
                eigen::plusTimes(k2[eq].data(), k1[eq].data(), 0.5, numNodes);
            /** [2] Step R-K */
            mesh.updateFlux(k2, Flux, config.v0, config.c0, config.rho0);
            numStep(mesh, config, k2, Flux, 0);
            for(int eq=0; eq<u.size(); ++eq)
                eigen::plusTimes(k3[eq].data(), k2[eq].data(), 0.5, numNodes);
            /** [3] Step R-K */
            mesh.updateFlux(k3, Flux, config.v0, config.c0, config.rho0);
            numStep(mesh, config, k3, Flux, 0);
            for(int eq=0; eq<u.size(); ++eq)
                eigen::plusTimes(k4[eq].data(), k3[eq].data(), 1, numNodes);
            /** [4] Step R-K */
            mesh.updateFlux(k4, Flux, config.v0, config.c0, config.rho0);
            numStep(mesh, config, k4, Flux, 0);
            /** Concat results of R-K iterations */
            for(int eq=0; eq<u.size(); ++eq){
                for(int i=0; i<numNodes; ++i){
                    u[eq][i] += (k1[eq][i]+2*k2[eq][i]+2*k3[eq][i]+k4[eq][i])/6.0;
                }
            }
        }

        /** Save to file */
        gmsh::view::write(gp_viewTag, config.saveFile, true);
        gmsh::view::write(grho_viewTag, config.saveFile, true);
        gmsh::view::write(gv_viewTag, config.saveFile, true);
    }
}
