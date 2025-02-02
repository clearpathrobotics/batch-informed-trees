/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2014, University of Toronto
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the University of Toronto nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Authors: Jonathan Gammell */

//Me!
#include "ompl/geometric/planners/bitstar/BITstar.h"

//For, you know, math
#include <cmath>
//For stringstreams
#include <sstream>
//For stream manipulations
#include <iomanip>
//For boost make_shared
#include <boost/make_shared.hpp>
//For boost::bind
#include <boost/bind.hpp>
//For pre C++ 11 gamma function
#include <boost/math/special_functions/gamma.hpp>

//For OMPL_INFORM et al.
#include "ompl/util/Console.h"
//For exceptions:
#include "ompl/util/Exception.h"
//For ompl::base::GoalStates:
#include "ompl/base/goals/GoalState.h"
//For getDefaultNearestNeighbors
#include "ompl/tools/config/SelfConfig.h"
//For ompl::geometric::path
#include "ompl/geometric/PathGeometric.h"
//For the default optimization objective:
#include "ompl/base/objectives/PathLengthOptimizationObjective.h"



namespace ompl
{
    namespace geometric
    {
        BITstar::BITstar(const ompl::base::SpaceInformationPtr& si, const std::string& name /*= "BITstar"*/)
            : ompl::base::Planner(si, name),
            sampler_(),
            opt_(),
            startVertex_(),
            goalVertex_(),
            freeStateNN_(),
            vertexNN_(),
            intQueue_(),
            sampleDensity_(0.0),
            r_(0.0), //Purposeful Gibberish
            k_rgg_(0.0), //Purposeful Gibberish
            k_(0u), //Purposeful Gibberish
            bestCost_( std::numeric_limits<double>::infinity() ), //Gets set in setup to the proper calls from OptimizationObjective
            prunedCost_( std::numeric_limits<double>::infinity() ), //Gets set in setup to the proper calls from OptimizationObjective
            minCost_( 0.0 ), //Gets set in setup to the proper calls from OptimizationObjective
            costSampled_(0.0), //Gets set in setup to the proper calls from OptimizationObjective
            hasSolution_(false),
            approximateSoln_(false),
            approximateDiff_(-1.0),
            numIterations_(0u),
            numBatches_(0u),
            numPrunings_(0u),
            numSamples_(0u),
            numVertices_(0u),
            numFreeStatesPruned_(0u),
            numVerticesDisconnected_(0u),
            numRewirings_(0u),
            numStateCollisionChecks_(0u),
            numEdgeCollisionChecks_(0u),
            numNearestNeighbours_(0u),
            useStrictQueueOrdering_(false),
            rewireFactor_(1.1),
            samplesPerBatch_(100u),
            useFailureTracking_(false),
            useKNearest_(false),
            usePruning_(true),
            pruneFraction_(0.01),
            stopOnSolnChange_(false)
        {
            //Specify my planner specs:
            Planner::specs_.recognizedGoal = ompl::base::GOAL_STATE;
            Planner::specs_.multithreaded = false;
            Planner::specs_.approximateSolutions = false; //For now!
            Planner::specs_.optimizingPaths = true;
            Planner::specs_.directed = true;
            Planner::specs_.provingSolutionNonExistence = false;

            OMPL_INFORM("%s: TODO: Implement goal-region support.", Planner::getName().c_str());
            OMPL_INFORM("%s: TODO: Implement approximate solution support.", Planner::getName().c_str());

            //Register my setting callbacks
            Planner::declareParam<bool>("use_strict_queue_ordering", this, &BITstar::setStrictQueueOrdering, &BITstar::getStrictQueueOrdering, "0,1");
            Planner::declareParam<double>("rewire_factor", this, &BITstar::setRewireFactor, &BITstar::getRewireFactor, "1.0:0.01:2.0");
            Planner::declareParam<unsigned int>("samples_per_batch", this, &BITstar::setSamplesPerBatch, &BITstar::getSamplesPerBatch, "1u:1u:1000000u");
            Planner::declareParam<bool>("use_edge_failure_tracking", this, &BITstar::setUseFailureTracking, &BITstar::getUseFailureTracking, "0,1");
            Planner::declareParam<bool>("use_k_nearest", this, &BITstar::setKNearest, &BITstar::getKNearest, "0,1");
            Planner::declareParam<bool>("use_graph_pruning", this, &BITstar::setPruning, &BITstar::getPruning, "0,1");
            Planner::declareParam<double>("prune_threshold_as_fractional_cost_change", this, &BITstar::setPruneThresholdFraction, &BITstar::getPruneThresholdFraction, "0.0:0.01:1.0");
            Planner::declareParam<bool>("stop_on_each_solution_improvement", this, &BITstar::setStopOnSolnImprovement, &BITstar::getStopOnSolnImprovement, "0,1");

            //Register my progress info:
            addPlannerProgressProperty("best cost DOUBLE", boost::bind(&BITstar::bestCostProgressProperty, this));
            addPlannerProgressProperty("current free states INTEGER", boost::bind(&BITstar::currentFreeProgressProperty, this));
            addPlannerProgressProperty("current vertices INTEGER", boost::bind(&BITstar::currentVertexProgressProperty, this));
            addPlannerProgressProperty("vertex queue size INTEGER", boost::bind(&BITstar::vertexQueueSizeProgressProperty, this));
            addPlannerProgressProperty("edge queue size INTEGER", boost::bind(&BITstar::edgeQueueSizeProgressProperty, this));
            addPlannerProgressProperty("iterations INTEGER", boost::bind(&BITstar::iterationProgressProperty, this));
            addPlannerProgressProperty("batches INTEGER", boost::bind(&BITstar::batchesProgressProperty, this));
            addPlannerProgressProperty("graph prunings INTEGER", boost::bind(&BITstar::pruningProgressProperty, this));
            addPlannerProgressProperty("total states generated INTEGER", boost::bind(&BITstar::totalStatesCreatedProgressProperty, this));
            addPlannerProgressProperty("vertices constructed INTEGER", boost::bind(&BITstar::verticesConstructedProgressProperty, this));
            addPlannerProgressProperty("states pruned INTEGER", boost::bind(&BITstar::statesPrunedProgressProperty, this));
            addPlannerProgressProperty("graph vertices disconnected INTEGER", boost::bind(&BITstar::verticesDisconnectedProgressProperty, this));
            addPlannerProgressProperty("rewiring edges INTEGER", boost::bind(&BITstar::rewiringProgressProperty, this));
            addPlannerProgressProperty("state collision checks INTEGER", boost::bind(&BITstar::stateCollisionCheckProgressProperty, this));
            addPlannerProgressProperty("edge collision checks INTEGER", boost::bind(&BITstar::edgeCollisionCheckProgressProperty, this));
            addPlannerProgressProperty("nearest neighbour calls INTEGER", boost::bind(&BITstar::nearestNeighbourProgressProperty, this));
        }



        BITstar::~BITstar()
        {
        }



        void BITstar::setup()
        {
            //Call the base class setup:
            Planner::setup();

            //Do some sanity checks
            //Make sure we have a problem definition
            if(Planner::pdef_ == false)
            {
                OMPL_ERROR("%s::setup() was called without a problem definition.", Planner::getName().c_str());
                Planner::setup_ = false;
                return;
            }

            //Make sure the problem only has one start state.
            if (Planner::pdef_->getStartStateCount() != 1u)
            {
                OMPL_ERROR("%s::setup() was called with %u start states, instead of exactly 1.", Planner::getName().c_str(), Planner::pdef_->getStartStateCount());
                Planner::setup_ = false;
                return;
            }

            //Make sure we have an optimization objective
            if (Planner::pdef_->hasOptimizationObjective() == false)
            {
                OMPL_INFORM("%s: No optimization objective specified. Defaulting to optimizing path length.", Planner::getName().c_str());
                Planner::pdef_->setOptimizationObjective( boost::make_shared<base::PathLengthOptimizationObjective> (Planner::si_) );
            }

            //Store the optimization objective for future ease of use
            opt_ = Planner::pdef_->getOptimizationObjective();

            //Configure the nearest-neighbour constructs.
            //Only allocate if they are empty (as they can be set to a specific version by a call to setNearestNeighbors)
            if (bool(freeStateNN_) == false)
            {
                freeStateNN_.reset( ompl::tools::SelfConfig::getDefaultNearestNeighbors<VertexPtr>(Planner::si_->getStateSpace()) );
            }
            //No else, already allocated (by a call to setNearestNeighbors())

            if (bool(vertexNN_) == false)
            {
                vertexNN_.reset( ompl::tools::SelfConfig::getDefaultNearestNeighbors<VertexPtr>(Planner::si_->getStateSpace()) );
            }
            //No else, already allocated (by a call to setNearestNeighbors())

            //Configure:
            freeStateNN_->setDistanceFunction(boost::bind(&BITstar::nnDistance, this, _1, _2));
            vertexNN_->setDistanceFunction(boost::bind(&BITstar::nnDistance, this, _1, _2));

            //Create the start as a vertex:
            startVertex_ = boost::make_shared<Vertex>(Planner::si_, opt_, true);

            //Copy the value of the start
            Planner::si_->copyState(startVertex_->state(), pdef_->getStartState(0u));

            //Create the goal as a vertex:
            //Create a vertex
            goalVertex_ = boost::make_shared<Vertex>(Planner::si_, opt_);

            //Copy the value of the goal
            Planner::si_->copyState(goalVertex_->state(), Planner::pdef_->getGoal()->as<ompl::base::GoalState>()->getState());

            //Configure the queue
            //boost::make_shared can only take 9 arguments, so be careful:
            intQueue_ = boost::make_shared<IntegratedQueue> (startVertex_, goalVertex_, boost::bind(&BITstar::nearestSamples, this, _1, _2), boost::bind(&BITstar::nearestVertices, this, _1, _2), boost::bind(&BITstar::lowerBoundHeuristicVertex, this, _1), boost::bind(&BITstar::currentHeuristicVertex, this, _1), boost::bind(&BITstar::lowerBoundHeuristicEdge, this, _1), boost::bind(&BITstar::currentHeuristicEdge, this, _1), boost::bind(&BITstar::currentHeuristicEdgeTarget, this, _1));
            intQueue_->setUseFailureTracking(useFailureTracking_);

            //Allocate a sampler:
            sampler_ = opt_->allocInformedStateSampler(Planner::si_->getStateSpace().get(), Planner::pdef_, &bestCost_);

            //Set the best-cost and pruned-cost to the proper opt_-based values:
            bestCost_ = opt_->infiniteCost();
            prunedCost_ = opt_->infiniteCost();

            //Set the minimum cost as the cost-to-come of the goal:
            minCost_ = this->costToComeHeuristic(goalVertex_);

            //Set the cost sampled to the maximum. This forces us to first check the basic start-goal graph.
            costSampled_ = bestCost_;

            //Finally, as they depend on some of the above, insert the start and goal into the proper sets
            //Add the goal to the set of samples.:
            this->addSample(goalVertex_);

            //Store the start into the tree and the queue:
            this->addVertex(startVertex_, false, true);

            //Finally initialize the nearestNeighbour terms:
            this->initializeNearestTerms();

            //Debug: Output an estimate of the state measure:
//            this->estimateMeasures();
        }



        void BITstar::clear()
        {
            //Clear all the variables.
            //Keep this in the order of the constructors list:

            //The various convenience pointers:
            sampler_.reset();
            opt_.reset();
            startVertex_.reset();
            goalVertex_.reset();

            //The list of samples
            if (bool(freeStateNN_) == true)
            {
                freeStateNN_->clear();
                freeStateNN_.reset();
            }
            //No else, not allocated

            //The list of vertices
            if (bool(vertexNN_) == true)
            {
                vertexNN_->clear();
                vertexNN_.reset();
            }

            //The queue:
            if (bool(intQueue_) == true)
            {
                intQueue_->clear();
                intQueue_.reset();
            }

            //DO NOT reset the parameters:
            //useStrictQueueOrdering_
            //rewireFactor_
            //samplesPerBatch_
            //useFailureTracking_
            //useKNearest_
            //usePruning_
            //pruneFraction_
            //stopOnSolnChange_

            //Reset the various calculations? TODO: Should I recalculate them?
            sampleDensity_ = 0.0;
            r_ = 0.0;
            k_rgg_ = 0.0; //This is a double for better rounding later
            k_ = 0u;
            bestCost_ = ompl::base::Cost(std::numeric_limits<double>::infinity());
            prunedCost_ = ompl::base::Cost(std::numeric_limits<double>::infinity());
            minCost_ = ompl::base::Cost(0.0);
            costSampled_ = ompl::base::Cost(0.0);
            hasSolution_ = false;
            approximateSoln_ = false;
            approximateDiff_ = -1.0;
            numIterations_ = 0u;
            numSamples_ = 0u;
            numVertices_ = 0u;
            numFreeStatesPruned_ = 0u;
            numVerticesDisconnected_ = 0u;
            numStateCollisionChecks_ = 0u;
            numEdgeCollisionChecks_ = 0u;
            numNearestNeighbours_ = 0u;
            numRewirings_ = 0u;
            numBatches_ = 0u;
            numPrunings_ = 0u;

            //Mark as not setup:
            setup_ = false;

            //Call my base clear:
            Planner::clear();
        }



        ompl::base::PlannerStatus BITstar::solve(const ompl::base::PlannerTerminationCondition& ptc)
        {
            Planner::checkValidity();
            OMPL_INFORM("%s: Searching for a solution to the given planning problem.", Planner::getName().c_str());
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start solve");
            //Variable
            //A manual stop to the iteration loop:
            bool stopLoop = false;

            //Run the outerloop until we're stopped, a suitable cost is found, or until we find the minimum possible cost within tolerance:
            while (opt_->isSatisfied(bestCost_) == false && ptc == false && this->isCostBetterThan(minCost_, bestCost_) == true && stopLoop == false)
            {
                //Variables:
                //The current edge:
                vertex_pair_t bestEdge;

                //Info:
                ++numIterations_;
                this->statusMessage(ompl::msg::LOG_DEBUG, "Iterate");

                //If we're using strict queue ordering, make sure the queues are up to date
                if(useStrictQueueOrdering_ == true)
                {
                    //The queues will be resorted if the graph has been rewired.
                    this->resort();
                }

                //If the edge queue is empty, that must mean we're either starting from scratch, or just finished a batch. Either way, make a batch of samples and fill the queue for the first time:
                if (intQueue_->isEmpty() == true)
                {
                    this->newBatch();
                }
                //No else, there is existing work to do!

                //Pop the minimum edge
                intQueue_->popFrontEdge(bestEdge);

                //In the best case, can this edge improve our solution given the current graph?
                //g_t(v) + c_hat(v,x) + h_hat(x) < g_t(x_g) )
                if (this->isCostBetterThan( opt_->combineCosts(bestEdge.first->getCost(), this->edgeCostHeuristic(bestEdge), this->costToGoHeuristic(bestEdge.second)), goalVertex_->getCost() ) == true)
                {
                    //Variables:
                    //The true cost of the edge:
                    ompl::base::Cost trueEdgeCost;

                    //Get the true cost of the edge (Is this the correct way?):
                    trueEdgeCost = this->trueEdgeCost(bestEdge);

                    //Can this actual edge ever improve our solution?
                    //g_hat(v) + c(v,x) + h_hat(x) < g_t(x_g)
                    if (this->isCostBetterThan( opt_->combineCosts(this->costToComeHeuristic(bestEdge.first), trueEdgeCost, this->costToGoHeuristic(bestEdge.second)),  goalVertex_->getCost() ) == true)
                    {
                        //Does this edge have a collision?
                        if (this->checkEdge(bestEdge) == true)
                        {
                            //Does the current edge improve our graph?
                            //g_t(v) + c(v,x) < g_t(x)
                            if (this->isCostBetterThan( opt_->combineCosts(bestEdge.first->getCost(), trueEdgeCost), bestEdge.second->getCost() ) == true)
                            {
                                //YAAAAH. Add the edge! Allowing for the sample to be removed from free (if appropriate) and the vertex queue updated (if appropriate)
                                this->addEdge(bestEdge, trueEdgeCost, true, true);

                                //Check for improved solution
                                if (this->isCostBetterThan(goalVertex_->getCost(), bestCost_) == true)
                                {
                                    //We have a better solution!
                                    if (hasSolution_ == false)
                                    {
                                        approximateSoln_ = false;
                                        approximateDiff_ = -1.0;
                                    }

                                    //Mark that we have a solution
                                    hasSolution_ = true;

                                    //Update the best cost:
                                    bestCost_ = goalVertex_->getCost();

                                    //Update the queue:
                                    intQueue_->setThreshold(bestCost_);

                                    //We will only prune the graph/samples on a new batch:

                                    //Mark to stop if necessary
                                    stopLoop = stopOnSolnChange_;

                                    OMPL_INFORM("%s: Found a solution with a cost of %.4f in %u iterations (%u vertices, %u rewirings). Graph currently has %u vertices.", Planner::getName().c_str(), goalVertex_->getCost(), numIterations_, numVertices_, numRewirings_, vertexNN_->size());
                                }
                                //No else

                                //Prune the edge queue of any unnecessary incoming edges
                                intQueue_->pruneEdgesTo(bestEdge.second);
                            }
                            //No else, this edge may be useful at some later date.
                        }
                        else if (useFailureTracking_ == true)
                        {
                            //If the edge failed, and we're tracking failures, record.
                            //This edge has a collision and can never be helpful. Poor edge. Add the target to the list of failed children for the source:
                            bestEdge.first->markAsFailedChild(bestEdge.second);
                        }
                        //No else, we failed and we're not tracking those
                    }
                    else if (useFailureTracking_ == true)
                    {
                        //If the edge failed, and we're tracking failures, record.
                        //This edge either has a very high edge cost and can never be helpful. Poor edge. Add the target to the list of failed children for the source
                        bestEdge.first->markAsFailedChild(bestEdge.second);
                    }
                    //No else, we failed and we're not tracking those
                }
                else if (intQueue_->isSorted() == false)
                {
                    //The edge cannot improve our solution, but the queue is imperfectly sorted, so we must resort before we give up.
                    this->resort();
                }
                else
                {
                    this->statusMessage(ompl::msg::LOG_DEBUG, "Clearing queue!");
                    //Else, I cannot improve the current solution, and as the queue is perfectly sorted and I am the best edge, no one can improve the current solution . Give up on the batch:
                    intQueue_->finish();
                }
            }

            if (hasSolution_ == true)
            {
                OMPL_INFORM("%s: Found a final solution of cost %.4f from %u samples by using %u vertices and %u rewirings. Final graph has %u vertices.", Planner::getName().c_str(), bestCost_.value(), numSamples_, numVertices_, numRewirings_, vertexNN_->size());

                this->publishSolution();
            }
            else
            {
                OMPL_INFORM("%s: Did not find a solution from %u samples after %u iterations, %u vertices and %u rewirings.", Planner::getName().c_str(), numSamples_, numIterations_, numVertices_, numRewirings_);
            }

            this->statusMessage(ompl::msg::LOG_DEBUG, "End solve");

            //PlannerStatus(addedSolution, approximate)
            return ompl::base::PlannerStatus(hasSolution_, approximateSoln_);
        }



        void BITstar::getPlannerData(ompl::base::PlannerData& data) const
        {
            //Get the base planner class data:
            Planner::getPlannerData(data);

            //Add samples
            if (freeStateNN_)
            {
                //Variables:
                //The list of unused samples:
                std::vector<VertexPtr> samples;

                //Get the list of samples
                freeStateNN_->list(samples);

                //Iterate through it turning each into a disconnected vertex
                for (std::vector<VertexPtr>::const_iterator sIter = samples.begin(); sIter != samples.end(); ++sIter)
                {
                    //No, add as a regular vertex:
                    data.addVertex(ompl::base::PlannerDataVertex((*sIter)->state()));
                }
            }
            //No else.

            //Add vertices
            if (vertexNN_)
            {
                //Variables:
                //The list of vertices in the graph:
                std::vector<VertexPtr> vertices;

                //Get the list of vertices
                vertexNN_->list(vertices);

                //Iterate through it turning each into a vertex with an edge:
                for (std::vector<VertexPtr>::const_iterator vIter = vertices.begin(); vIter != vertices.end(); ++vIter)
                {
                    //Is the vertex the start?
                    if ((*vIter)->isRoot() == true)
                    {
                        //Yes, add as a start vertex:
                        data.addStartVertex(ompl::base::PlannerDataVertex((*vIter)->state()));
                    }
                    else
                    {
                        //No, add as a regular vertex:
                        data.addVertex(ompl::base::PlannerDataVertex((*vIter)->state()));

                        //And as an incoming edge
                        data.addEdge(ompl::base::PlannerDataVertex((*vIter)->getParent()->state()), ompl::base::PlannerDataVertex((*vIter)->state()));
                    }
                }
            }
            //No else.

            //Did we find a solution?
            if (hasSolution_ == true)
            {
                data.markGoalState(goalVertex_->state());
            }

            data.properties["best_solution_cost DOUBLE"] = this->bestCostProgressProperty();
            data.properties["current_number_of_free_states INTEGER"] = this->currentFreeProgressProperty();
            data.properties["current_number_of_graph_vertices INTEGER"] = this->currentVertexProgressProperty();
            data.properties["current_vertex_queue_size INTEGER"] = this->vertexQueueSizeProgressProperty();
            data.properties["current_edge_queue_size INTEGER"] = this->edgeQueueSizeProgressProperty();
            data.properties["iterations INTEGER"] = this->iterationProgressProperty();
            data.properties["number_of_batches INTEGER"] = this->batchesProgressProperty();
            data.properties["number_of_graph_prunings INTEGER"] = this->pruningProgressProperty();
            data.properties["total_states_generated INTEGER"] = this->totalStatesCreatedProgressProperty();
            data.properties["total_vertices_added_to_the_graph INTEGER"] = this->verticesConstructedProgressProperty();
            data.properties["states_pruned_from_problem INTEGER"] = this->statesPrunedProgressProperty();
            data.properties["graph_vertices_disconnected INTEGER"] = this->verticesDisconnectedProgressProperty();
            data.properties["rewiring_edges_performed INTEGER"] = this->rewiringProgressProperty();
            data.properties["number_of_state_collision_checks INTEGER"] = this->stateCollisionCheckProgressProperty();
            data.properties["number_of_edge_collision_checks INTEGER"] = this->edgeCollisionCheckProgressProperty();
            data.properties["number_of_nearest_neighbour_calls INTEGER"] = this->nearestNeighbourProgressProperty();
        }



        std::pair<ompl::base::State*, ompl::base::State*> BITstar::getNextEdgeInQueue()
        {
            //Variable:
            //The next edge as a basic pair of states
            std::pair<ompl::base::State*, ompl::base::State*> nextEdge;

            //If we're using strict queue ordering, make sure the queue is up to date
            if(useStrictQueueOrdering_ == true)
            {
                //Resort the queues as necessary (if the graph has been rewired).
                this->resort();
            }

            if (intQueue_->isEmpty() == false)
            {
                //The next edge in the queue:
                nextEdge = std::make_pair(intQueue_->frontEdge().first->state(), intQueue_->frontEdge().second->state());
            }
            else
            {
                //An empty edge:
                nextEdge = std::make_pair<ompl::base::State*, ompl::base::State*>(NULL, NULL);
            }

            return nextEdge;
        }



        ompl::base::Cost BITstar::getNextEdgeValueInQueue()
        {
            //Variable
            //The cost of the next edge
            ompl::base::Cost nextCost;

            //If we're using strict queue ordering, make sure the queue is up to date
            if(useStrictQueueOrdering_ == true)
            {
                //Resort the queues as necessary (if the graph has been rewired).
                this->resort();
            }

            if (intQueue_->isEmpty() == false)
            {
                //The next cost in the queue:
                nextCost = intQueue_->frontEdgeValue().first;
            }
            else
            {
                //An infinite cost:
                nextCost =  opt_->infiniteCost();
            }

            return nextCost;
        }



        void BITstar::getEdgeQueue(std::vector<std::pair<VertexPtr, VertexPtr> >& edgesInQueue)
        {
            intQueue_->listEdges(&edgesInQueue);
        }



        void BITstar::getVertexQueue(std::vector<VertexPtr>& verticesInQueue)
        {
            intQueue_->listVertices(&verticesInQueue);
        }



        template<template<typename T> class NN>
        void BITstar::setNearestNeighbors()
        {
            //Check if the problem is already setup, if so, the NN structs have data in them and you can't really change them:
            if (Planner::setup_ == true)
            {
                throw ompl::Exception("The type of nearest neighbour datastructure cannot be changed once a planner is setup. ");
            }
            else
            {
                //The problem isn't setup yet, create NN structs of the specified type:
                freeStateNN_ = boost::make_shared< NN<VertexPtr> >();
                vertexNN_ = boost::make_shared< NN<VertexPtr> >();
            }
        }



















        void BITstar::estimateMeasures()
        {
            OMPL_INFORM("%s: Estimating the measure of the planning domain. This is a debugging function that does not have any effect on the planner.", Planner::getName().c_str());
            //Variables:
            //The total number of samples:
            unsigned int numTotalSamples;
            //The resulting samples in free:
            unsigned int numFreeSamples;
            //The resulting samples in obs:
            unsigned int numObsSamples;
            //The sample fraction of free:
            double fractionFree;
            //The sample fraction of obs:
            double fractionObs;
            //The total measure of the space:
            double totalMeasure;
            //The resulting estimate of the free measure
            double freeMeasure;
            //The resulting estimate of the obs measure
            double obsMeasure;

            //Set the total number of samples
            numTotalSamples = 100000u;
            numFreeSamples = 0u;
            numObsSamples = 0u;

            //Draw samples, classifying each one
            for (unsigned int i = 0u; i < numTotalSamples; ++i)
            {
                //Allocate a state
                ompl::base::State* aState = Planner::si_->allocState();

                //Sample:
                sampler_->sampleUniform(aState);

                //Check if collision free
                if (Planner::si_->isValid(aState) == true)
                {
                    ++numFreeSamples;
                }
                else
                {
                    ++numObsSamples;
                }
            }

            //Calculate the fractions:
            fractionFree = static_cast<double>(numFreeSamples)/static_cast<double>(numTotalSamples);

            fractionObs = static_cast<double>(numObsSamples)/static_cast<double>(numTotalSamples);

            //Get the total measure of the space
            totalMeasure = Planner::si_->getSpaceMeasure();

            //Calculate the measure of the free space
            freeMeasure = fractionFree*totalMeasure;

            //Calculate the measure of the obs space
            obsMeasure = fractionObs*totalMeasure;

            //Announce
            OMPL_INFORM("%s: %u samples (%u free, %u in collision) from a space with measure %.4f estimates %.2f%% free and %.2f%% in collision (measures of %.4f and %.4f, respectively).", Planner::getName().c_str(), numTotalSamples, numFreeSamples, numObsSamples, totalMeasure, 100.0*fractionFree, 100.0*fractionObs, freeMeasure, obsMeasure);
        }



        void BITstar::newBatch()
        {
            //Variable:
            //The states as a vector:
            std::vector<VertexPtr> vertices;

            //Info:
            ++numBatches_;
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start new batch.");

            //Set the cost sampled to the minimum
            costSampled_ = minCost_;

            /* Clearing the samples would invalidate the uniform-density assumptions of the RGG:
            //Clear the samples?
            freeStateNN_->clear();
            */

            //Reset the queue:
            intQueue_->reset();

            //Prune the graph (if enabled)
            this->prune();

            //Calculate the sampling density (currently unused but for eventual JIT sampling)
            sampleDensity_ = static_cast<double>(samplesPerBatch_)/sampler_->getInformedMeasure();

            this->statusMessage(ompl::msg::LOG_DEBUG, "End new batch.");
        }



        void BITstar::updateSamples(const VertexPtr& vertex)
        {
            //Info:
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start update samples");

            //Check if we need to sample (This is in preparation for JIT sampling:)
            if (this->isCostBetterThan(costSampled_, bestCost_))
            {
                //Update the sampler counter:
                numSamples_ = numSamples_ + samplesPerBatch_;

                //Generate samples
                for (unsigned int i = 0u; i < samplesPerBatch_; ++i)
                {
                    //Variable
                    //The new state:
                    VertexPtr newState = boost::make_shared<Vertex>(Planner::si_, opt_);

                    //Sample:
                    sampler_->sampleUniform(newState->state());

                    //If the state is collision free, add it to the list of free states
                    //We're counting density in the total state space, not free space
                    ++numStateCollisionChecks_;
                    if (Planner::si_->isValid(newState->state()) == true)
                    {
                        //Add the new state as a sample
                        this->addSample(newState);
                    }
                }

                //Mark that we've sampled all cost spaces (This is in preparation for JIT sampling)
                costSampled_ = opt_->infiniteCost();

                //Finally, update the nearest-neighbour terms
                this->updateNearestTerms();
            }

            ///JIT Sampling code. To return to:
            /*
            //Variables:
            //The required cost to sample to
            ompl::base::Cost reqCost;

            //The required cost is the minimum between the cost required to encompass our local neighbourhood and the current solution cost
            reqCost = this->betterCost(opt_->combineCosts(this->lowerBoundHeuristicVertex(vertex), this->neighbourhoodCost()), bestCost_);

            //Check if we've sampled this cost space yet
            if (this->isCostBetterThan(costSampled_, reqCost))
            //We haven't, so we must sample the new shell in cost space
            {
                //Variable:
                //The volume of the space we're sampling:
                double sampleMeasure;
                //The resulting number of samples, as a double:
                double dblNumSamples;
                //The number of samples as an unsigned int;
                unsigned int uintNumSamples;

                //Calculate the volume of the space under consideration:
                sampleMeasure = sampler_->getHypotheticalMeasure(reqCost) - sampler_->getHypotheticalMeasure(costSampled_);

                //Calculate the number of samples necessary:
                dblNumSamples = sampleDensity_*sampleMeasure;

                //Probabilistically round the double to an uint
                uintNumSamples = static_cast<unsigned int>(dblNumSamples);
                if ( sampler_->rng().uniform01() < (dblNumSamples - static_cast<double>(uintNumSamples) ) )
                {
                    uintNumSamples = uintNumSamples + 1;
                }

                OMPL_DEBUG("%s:     Generating %u samples.", Planner::getName().c_str(), uintNumSamples);

                //Update the sampler counter:
                numSamples_ = numSamples_ + uintNumSamples;

                //Sample the shell of the state space that many times, adding the valid ones to the list of free states.
                for (unsigned int i = 0u; i < uintNumSamples; ++i)
                {
                    //Variable
                    //The new state:
                    VertexPtr newState = boost::make_shared<Vertex>(Planner::si_, opt_);

                    //Sample:
                    sampler_->sampleUniformIgnoreBounds(newState->state(), costSampled_, reqCost);

                    //If the state is collision free, add it to the list of free states:
                    ++numStateCollisionChecks_
                    if (Planner::si_->isValid(newState->state()) == true)
                    {
                        //Add
                        this->addSample(newState);
                    }
                    //No else, we're counting density in the total state space, not free space
                }

                //Update the sampled cost:
                costSampled_ = reqCost;
            }
            //No else, we've sampled this cost-space already

            //Update the radius, it will only get smaller, so we were simply conservative by not updating it before:
            this->updateNearestTerms();
            */

            this->statusMessage(ompl::msg::LOG_DEBUG, "End update samples");
        }



        void BITstar::prune()
        {
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start pruning.");

            //Test if we should we do a little tidying up:
            //Is pruning enabled? Do we have a solution? Has the solution changed enough?
            if ( (usePruning_ == true) && (hasSolution_ == true) && (std::abs(this->fractionalChange(bestCost_, prunedCost_)) > pruneFraction_) )
            {
                //Is there good reason to prune? I.e., is the informed subset measurably less than the total problem domain? If an informed measure is not available, we'll assume yes:
                if ( (sampler_->hasInformedMeasure() == true && sampler_->getInformedMeasure() < si_->getSpaceMeasure()) || (sampler_->hasInformedMeasure() == false) )
                {
                    //Variable:
                    //The number of vertices and samples pruned
                    std::pair<unsigned int, unsigned int> numPruned;

                    OMPL_INFORM("%s: Pruning the planning problem from %.4f to %.4f.", Planner::getName().c_str(), prunedCost_, bestCost_);

                    //Increment the pruning counter:
                    ++numPrunings_;

                    //Prune the samples
                    this->pruneSamples();

                    //Prune the graph. This can be done extra efficiently by using some info in the integrated queue.
                    //This requires access to the nearest neighbour structures so vertices can be moved to free states.
                    numPruned = intQueue_->prune(vertexNN_, freeStateNN_);

                    //The number of vertices and samples pruned are incrementally updated.
                    numVerticesDisconnected_ = numVerticesDisconnected_ + numPruned.first;
                    numFreeStatesPruned_ = numFreeStatesPruned_ + numPruned.second;

                    //Store the cost at which we pruned:
                    prunedCost_ = bestCost_;
                }
                //No else, it's not worth the work to prune...
            }
            //No else, why was I called?

            this->statusMessage(ompl::msg::LOG_DEBUG, "End pruning.");
        }



        void BITstar::resort()
        {
            //Variable:
            //The number of vertices and samples pruned
            std::pair<unsigned int, unsigned int> numPruned;

            //Resorting requires access to the nearest neighbour structures so vertices can be pruned instead of resorted.
            //The number of vertices pruned is also incrementally updated.
            numPruned = intQueue_->resort(vertexNN_, freeStateNN_);

            //The number of vertices and samples pruned are incrementally updated.
            numVerticesDisconnected_ = numVerticesDisconnected_ + numPruned.first;
            numFreeStatesPruned_ = numFreeStatesPruned_ + numPruned.second;
        }



        void BITstar::publishSolution()
        {
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start publish solution.");

            //Variable
            //A vector of vertices from goal->start:
            std::vector<VertexPtr> reversePath;
            //The path geometric
            boost::shared_ptr<ompl::geometric::PathGeometric> pathGeoPtr;

            //Allocate the pathGeoPtr
            pathGeoPtr = boost::make_shared<ompl::geometric::PathGeometric>(Planner::si_);

            //Iterate up the chain from the goal, creating a backwards vector:
            for(VertexPtr vertex = goalVertex_; bool(vertex) == true; vertex = vertex->getParent())
            {
                reversePath.push_back(vertex);
            }

            //Now iterate that vector in reverse, putting the states into the path geometric
            for (std::vector<VertexPtr>::const_reverse_iterator vIter = reversePath.rbegin(); vIter != reversePath.rend(); ++vIter)
            {
                pathGeoPtr->append( (*vIter)->state() );
            }

            //Now create the solution
            ompl::base::PlannerSolution soln(pathGeoPtr);

            //Mark the name:
            soln.setPlannerName(Planner::getName());

            //Mark as exact or approximate:
            if (approximateSoln_ == true)
            {
                soln.setApproximate(approximateDiff_);
            }

            //Mark whether the solution met the optimization objective:
            soln.optimized_ = opt_->isSatisfied(bestCost_);

            //Add the solution to the Problem Definition:
            Planner::pdef_->addSolutionPath(soln);

            this->statusMessage(ompl::msg::LOG_DEBUG, "End publish solution.");
        }



        void BITstar::pruneSamples()
        {
            this->statusMessage(ompl::msg::LOG_DEBUG, "Start prune samples.");

            //Variable:
            //The list of samples:
            std::vector<VertexPtr> samples;

            //Get the list of samples
            freeStateNN_->list(samples);

            //Iterate through the list and remove any samples that have a heuristic larger than the bestCost_
            for (unsigned int i = 0u; i < samples.size(); ++i)
            {
                //Check if this state should be pruned:
                if (intQueue_->samplePruneCondition(samples.at(i)) == true)
                {
                    //Yes, remove it
                    this->dropSample(samples.at(i));
                }
                //No else, keep.
            }

            this->statusMessage(ompl::msg::LOG_DEBUG, "End prune samples.");
        }



        bool BITstar::checkEdge(const vertex_pair_t& edge)
        {
            ++numEdgeCollisionChecks_;
            return Planner::si_->checkMotion(edge.first->state(), edge.second->state());
        }



        void BITstar::dropSample(VertexPtr oldSample)
        {
            //Update the counter:
            ++numFreeStatesPruned_;

            //Remove from the list of samples
            freeStateNN_->remove(oldSample);
        }



        void BITstar::addEdge(const vertex_pair_t& newEdge, const ompl::base::Cost& edgeCost, const bool& removeFromFree, const bool& updateExpansionQueue)
        {
            //If the vertex is currently in the tree, we need to rewire
            if (newEdge.second->isConnected() == true)
            {
                //Replace the edge
                this->replaceParent(newEdge, edgeCost);
            }
            else
            {
                //If not, we just add the vertex, first connect:

                //Add a child to the parent, not updating costs:
                newEdge.first->addChild(newEdge.second, false);

                //Add a parent to the child, updating costs:
                newEdge.second->addParent(newEdge.first, edgeCost, true);

                //Then add to the queues as necessary
                this->addVertex(newEdge.second, removeFromFree, updateExpansionQueue);
            }
        }



        void BITstar::replaceParent(const vertex_pair_t& newEdge, const ompl::base::Cost& edgeCost)
        {
            //Increment our counter:
            ++numRewirings_;

            //Remove the child from the parent, not updating costs
            newEdge.second->getParent()->removeChild(newEdge.second, false);

            //Remove the parent from the child, not updating costs
            newEdge.second->removeParent(false);

            //Add the child to the parent, not updating costs
            newEdge.first->addChild(newEdge.second, false);

            //Add the parent to the child. This updates the cost of the child as well as all it's descendents.
            newEdge.second->addParent(newEdge.first, edgeCost, true);

            //Mark the queues as unsorted below this child
            intQueue_->markVertexUnsorted(newEdge.second);
        }



        void BITstar::addSample(const VertexPtr& newSample)
        {
            //Mark as new
            newSample->markNew();

            //Add to the NN structure:
            freeStateNN_->add(newSample);
        }



        void BITstar::addVertex(const VertexPtr& newVertex, const bool& removeFromFree, const bool& updateExpansionQueue)
        {
            //Make sure it's connected first, so that the queue gets updated properly. This is a day of debugging I'll never get back
            if (newVertex->hasParent() == false && newVertex->isRoot() == false)
            {
                throw ompl::Exception("Vertices must be connected to the graph before adding");
            }

            //Remove the vertex from the list of samples (if it even existed)
            if (removeFromFree == true)
            {
                freeStateNN_->remove(newVertex);
            }
            //No else

            //Add to the NN structure:
            vertexNN_->add(newVertex);

            //Should we update the vertex queue?
            if (updateExpansionQueue == true)
            {
                //Add to the queue:
                intQueue_->insertVertex(newVertex);
            }
            //No else.

            //Increment the number of vertices added:
            ++numVertices_;
        }



        double BITstar::nnDistance(const VertexPtr& a, const VertexPtr& b) const
        {
            //Using RRTstar as an example, this order gives us the distance FROM the queried state TO the other neighbours in the structure.
            //The distance function between two states
            if (!a->state())
            {
                throw ompl::Exception("a->state is unallocated");
            }
            if (!b->state())
            {
                throw ompl::Exception("b->state is unallocated");
            }
            return Planner::si_->distance(b->state(), a->state());
        }



        ompl::base::Cost BITstar::lowerBoundHeuristicVertex(const VertexPtr& vertex) const
        {
            return opt_->combineCosts( this->costToComeHeuristic(vertex), this->costToGoHeuristic(vertex) );
        }



        ompl::base::Cost BITstar::currentHeuristicVertex(const VertexPtr& vertex) const
        {
            return opt_->combineCosts( vertex->getCost(), this->costToGoHeuristic(vertex) );
        }


        ompl::base::Cost BITstar::lowerBoundHeuristicEdge(const vertex_pair_t& edgePair) const
        {
            return opt_->combineCosts(this->costToComeHeuristic(edgePair.first), this->edgeCostHeuristic(edgePair), this->costToGoHeuristic(edgePair.second));
        }



        ompl::base::Cost BITstar::currentHeuristicEdge(const vertex_pair_t& edgePair) const
        {
            return opt_->combineCosts(this->currentHeuristicEdgeTarget(edgePair), this->costToGoHeuristic(edgePair.second));
        }



        ompl::base::Cost BITstar::currentHeuristicEdgeTarget(const vertex_pair_t& edgePair) const
        {
            return opt_->combineCosts(edgePair.first->getCost(), this->edgeCostHeuristic(edgePair));
        }



        ompl::base::Cost BITstar::costToComeHeuristic(const VertexPtr& vertex) const
        {
            return opt_->motionCostHeuristic(startVertex_->state(), vertex->state());
        }



        ompl::base::Cost BITstar::edgeCostHeuristic(const vertex_pair_t& edgePair) const
        {
            return opt_->motionCostHeuristic(edgePair.first->state(), edgePair.second->state());
        }



        ompl::base::Cost BITstar::costToGoHeuristic(const VertexPtr& vertex) const
        {
            //opt_->costToGo(vertex->state(), Planner::pdef_->getGoal().get());
            return opt_->motionCostHeuristic(vertex->state(), goalVertex_->state());
        }


        ompl::base::Cost BITstar::trueEdgeCost(const vertex_pair_t& edgePair) const
        {
            return ompl::base::Cost(Planner::si_->distance(edgePair.first->state(), edgePair.second->state()));
        }



        ompl::base::Cost BITstar::neighbourhoodCost() const
        {
            OMPL_INFORM("%s: TODO: Write neighbourhoodCost() more generally.", Planner::getName().c_str());
            return ompl::base::Cost( 2.0*r_ );
        }



        bool BITstar::isCostBetterThan(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            return a.value() < b.value();
        }



        bool BITstar::isCostWorseThan(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            //If b is better than a, then a is worse than b
            return this->isCostBetterThan(b, a);
        }



        bool BITstar::isCostEquivalentTo(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            //If a is not better than b, and b is not better than a, then they are equal
            return !this->isCostBetterThan(a,b) && !this->isCostBetterThan(b,a);
        }



        bool BITstar::isCostNotEquivalentTo(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            //If a is better than b, or b is better than a, then they are not equal
            return this->isCostBetterThan(a,b) || this->isCostBetterThan(b,a);
        }



        bool BITstar::isCostBetterThanOrEquivalentTo(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            //If b is not better than a, then a is better than, or equal to, b
            return !this->isCostBetterThan(b, a);
        }



        bool BITstar::isCostWorseThanOrEquivalentTo(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            //If a is not better than b, than a is worse than, or equal to, b
            return !this->isCostBetterThan(a,b);
        }



        bool BITstar::isFinite(const ompl::base::Cost& cost) const
        {
            return this->isCostBetterThan(cost, opt_->infiniteCost());
        }



        ompl::base::Cost BITstar::betterCost(const ompl::base::Cost& a, const ompl::base::Cost& b) const
        {
            if (this->isCostBetterThan(b,a))
            {
                return b;
            }
            else
            {
                return a;
            }
        }




        double BITstar::fractionalChange(const ompl::base::Cost& newCost, const ompl::base::Cost& oldCost)
        {
            //If the old cost is not finite, than we call that infinite percent improvement
            if (this->isFinite(oldCost) == false)
            {
                //Return infinity (but not beyond)
                return std::numeric_limits<double>::infinity();
            }
            else
            {
                //Calculate and return
                return ( newCost.value() - oldCost.value() )/oldCost.value();
            }
        }



        void BITstar::nearestSamples(const VertexPtr& vertex, std::vector<VertexPtr>* neighbourSamples)
        {
            //Make sure sampling has happened first:
            this->updateSamples(vertex);

            //Increment our counter:
            ++numNearestNeighbours_;

            if (useKNearest_ == true)
            {
                freeStateNN_->nearestK(vertex, k_, *neighbourSamples);
            }
            else
            {
                freeStateNN_->nearestR(vertex, r_, *neighbourSamples);
            }
        }


        void BITstar::nearestVertices(const VertexPtr& vertex, std::vector<VertexPtr>* neighbourVertices)
        {
            //Increment our counter:
            ++numNearestNeighbours_;

            if (useKNearest_ == true)
            {
                vertexNN_->nearestK(vertex, k_, *neighbourVertices);
            }
            else
            {
                vertexNN_->nearestR(vertex, r_, *neighbourVertices);
            }
        }



        void BITstar::initializeNearestTerms()
        {
            //Calculate the k-nearest constant
            k_rgg_ = this->minimumRggK();

            this->updateNearestTerms();
        }



        void BITstar::updateNearestTerms()
        {
            //Variables:
            //The number of samples:
            unsigned int N;

            //Calculate the number of N:
            N = vertexNN_->size() + freeStateNN_->size();

            if (useKNearest_ == true)
            {
                k_ = this->k(N);
            }
            else
            {
                r_ = this->r(N);
            }

//            std::cout << "r(" << N << ") = " << r_ << std::endl;
        }



        double BITstar::r(unsigned int N) const
        {
            //Variables
            //The dimension cast as a double for readibility;
            double dimDbl = static_cast<double>(Planner::si_->getStateDimension());
            //The size of the graph
            double cardDbl = static_cast<double>(N);

            //Calculate the term and return
            return this->minimumRggR()*std::pow( std::log(cardDbl)/cardDbl, 1/dimDbl );
        }



        unsigned int BITstar::k(unsigned int N) const
        {
            //Calculate the term and return
            return std::ceil( k_rgg_ * std::log(static_cast<double>(N)) );
        }



        double BITstar::minimumRggR() const
        {
            //Variables
            //The dimension cast as a double for readibility;
            double dimDbl = static_cast<double>(Planner::si_->getStateDimension());

            //Calculate the term and return
            return rewireFactor_*2.0*std::pow( (1.0 + 1.0/dimDbl)*( sampler_->getInformedMeasure()/ompl::ProlateHyperspheroid::unitNBallMeasure(Planner::si_->getStateDimension()) ), 1.0/dimDbl ); //RRG radius (biggest for unit-volume problem)
            //return rewireFactor_*std::pow( 2.0*(1.0 + 1.0/dimDbl)*( sampler_->getInformedMeasure()/ompl::ProlateHyperspheroid::unitNBallMeasure(Planner::si_->getStateDimension()) ), 1.0/dimDbl ); //RRT* radius (smaller for unit-volume problem)
            //return rewireFactor_*2.0*std::pow( (1.0/dimDbl)*( sampler_->getInformedMeasure()/ompl::ProlateHyperspheroid::unitNBallMeasure(Planner::si_->getStateDimension()) ), 1.0/dimDbl ); //FMT* radius (smallest for R2, equiv to RRT* for R3 and then middle for higher d. All unit-volume)
        }



        double BITstar::minimumRggK() const
        {
            //Variables
            //The dimension cast as a double for readibility;
            double dimDbl = static_cast<double>(Planner::si_->getStateDimension());

            //Calculate the term and return
            return rewireFactor_*(boost::math::constants::e<double>() + (boost::math::constants::e<double>() / dimDbl)); //RRG k-nearest
        }





        void BITstar::statusMessage(const ompl::msg::LogLevel& msgLevel, const std::string& status) const
        {
            //Check if we need to create the message
            if (msgLevel >= ompl::msg::getLogLevel())
            {
                //Variable
                //The message as a stream:
                std::stringstream outputStream;

                //Create the stream:
                //The name of the planner
                outputStream << Planner::getName();
                outputStream << " (";
                //The current path cost:
                outputStream << "l: " << std::setw(6) << std::setfill(' ') << std::setprecision(5) << bestCost_.value();
                //The number of batches:
                outputStream << ", b: " << std::setw(5) << std::setfill(' ') << numBatches_;
                //The number of iterations
                outputStream << ", i: " << std::setw(5) << std::setfill(' ') << numIterations_;
                //The number of states current in the graph
                outputStream << ", g: " << std::setw(5) << std::setfill(' ') << vertexNN_->size();
                //The number of free states
                outputStream << ", f: " << std::setw(5) << std::setfill(' ') << freeStateNN_->size();
                //The number edges in the queue:
                outputStream << ", q: " << std::setw(5) << std::setfill(' ') << intQueue_->numEdges();
                //The number of samples generated
                outputStream << ", s: " << std::setw(5) << std::setfill(' ') << numSamples_;
                //The number of vertices ever added to the graph:
                outputStream << ", v: " << std::setw(5) << std::setfill(' ') << numVertices_;
                //The number of prunings:
                outputStream << ", p: " << std::setw(5) << std::setfill(' ') << numPrunings_;
                //The number of rewirings:
                outputStream << ", r: " << std::setw(5) << std::setfill(' ') << numRewirings_;
                //The number of nearest-neighbour calls
                outputStream << ", n: " << std::setw(5) << std::setfill(' ') << numNearestNeighbours_;
                //The number of state collision checks:
                outputStream << ", c(s): " << std::setw(5) << std::setfill(' ') << numStateCollisionChecks_;
                //The number of edge collision checks:
                outputStream << ", c(e): " << std::setw(5) << std::setfill(' ') << numEdgeCollisionChecks_;
                outputStream << "):    ";
                //The message:
                outputStream << status;


                if (msgLevel == ompl::msg::LOG_DEBUG)
                {
                    OMPL_DEBUG("%s", outputStream.str().c_str());
                }
                else if (msgLevel == ompl::msg::LOG_INFO)
                {
                    OMPL_INFORM("%s", outputStream.str().c_str());
                }
                else if (msgLevel == ompl::msg::LOG_WARN)
                {
                    OMPL_WARN("%s", outputStream.str().c_str());
                }
                else if (msgLevel == ompl::msg::LOG_ERROR)
                {
                    OMPL_ERROR("%s", outputStream.str().c_str());
                }
                else
                {
                    throw ompl::Exception("Log level not recognized");
                }
            }
            //No else, this message is below the log level
        }



        //****************** A bunch of boring gets/sets ******************//

        boost::uint32_t BITstar::getRngLocalSeed() const
        {
            if (sampler_)
            {
                return sampler_->getLocalSeed();
            }
            else
            {
                throw ompl::Exception("Sampler not yet allocated");
            }
        }



        void BITstar::setRngLocalSeed(boost::uint32_t seed)
        {
            if (sampler_)
            {
                sampler_->setLocalSeed(seed);
            }
            else
            {
                throw ompl::Exception("Sampler not yet allocated");
            }
        }



        void BITstar::setRewireFactor(double rewireFactor)
        {
            rewireFactor_ = rewireFactor;

            //Check if there's things to update
            if (this->isSetup() == true)
            {
                //Reinitialize the terms:
                this->initializeNearestTerms();
            }
        }



        double BITstar::getRewireFactor() const
        {
            return rewireFactor_;
        }



        void BITstar::setSamplesPerBatch(unsigned int n)
        {
            samplesPerBatch_ = n;
        }



        unsigned int BITstar::getSamplesPerBatch() const
        {
            return samplesPerBatch_;
        }



        void BITstar::setKNearest(bool useKNearest)
        {
            //Check if the flag has changed
            if (useKNearest != useKNearest_)
            {
                //Set the k-nearest flag
                useKNearest_ = useKNearest;

                if (useKNearest_ == true)
                {
                    //Warn that this isn't exactly implemented
                    OMPL_WARN("%s: The implementation of the k-Nearest version of BIT* is not 100%% correct.", Planner::getName().c_str()); //This is because we have a separate nearestNeighbours structure for samples and vertices and you don't know what fraction of K to ask for from each...
                }

                //Check if there's things to update
                if (this->isSetup() == true)
                {
                    //Reinitialize the terms:
                    this->initializeNearestTerms();
                }
            }
            //No else, it didn't change.
        }



        bool BITstar::getKNearest() const
        {
            return useKNearest_;
        }



        void BITstar::setUseFailureTracking(bool trackFailures)
        {
            //Store
            useFailureTracking_ = trackFailures;

            //Configure queue if constructed:
            if (intQueue_)
            {
                intQueue_->setUseFailureTracking(useFailureTracking_);
            }
        }



        bool BITstar::getUseFailureTracking() const
        {
            return useFailureTracking_;
        }


        void BITstar::setStrictQueueOrdering(bool beStrict)
        {
            useStrictQueueOrdering_ = beStrict;
        }



        bool BITstar::getStrictQueueOrdering() const
        {
            return useStrictQueueOrdering_;
        }



        void BITstar::setPruning(bool prune)
        {
            if (prune == false)
            {
                OMPL_WARN("%s: Turning pruning off does not turn a fake pruning on, as it should.", Planner::getName().c_str());
            }

            usePruning_ = prune;
        }



        bool BITstar::getPruning() const
        {
            return usePruning_;
        }



        void BITstar::setPruneThresholdFraction(double fractionalChange)
        {
            if (fractionalChange < 0.0 || fractionalChange > 1.0)
            {
                throw ompl::Exception("Prune threshold must be specified as a fraction between [0, 1].");
            }

            pruneFraction_ = fractionalChange;
        }



        double BITstar::getPruneThresholdFraction() const
        {
            return pruneFraction_;
        }



        void BITstar::setStopOnSolnImprovement(bool stopOnChange)
        {
            stopOnSolnChange_ = stopOnChange;
        }



        bool BITstar::getStopOnSolnImprovement() const
        {
            return stopOnSolnChange_;
        }



        ompl::base::Cost BITstar::bestCost() const
        {
            return bestCost_;
        }



        std::string BITstar::bestCostProgressProperty() const
        {
            return boost::lexical_cast<std::string>(this->bestCost().value());
        }



        std::string BITstar::currentFreeProgressProperty() const
        {
            return boost::lexical_cast<std::string>(freeStateNN_->size());
        }



        std::string BITstar::currentVertexProgressProperty() const
        {
            return boost::lexical_cast<std::string>(vertexNN_->size());
        }



        std::string BITstar::vertexQueueSizeProgressProperty() const
        {
            return boost::lexical_cast<std::string>(intQueue_->numVertices());
        }



        std::string BITstar::edgeQueueSizeProgressProperty() const
        {
            return boost::lexical_cast<std::string>(intQueue_->numEdges());
        }



        std::string BITstar::iterationProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numIterations_);
        }


        unsigned int BITstar::numBatches() const
        {
            return numBatches_;
        }



        std::string BITstar::batchesProgressProperty() const
        {
            return boost::lexical_cast<std::string>(this->numBatches());
        }



        std::string BITstar::pruningProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numPrunings_);
        }



        std::string BITstar::totalStatesCreatedProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numSamples_);
        }



        std::string BITstar::verticesConstructedProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numVertices_);
        }



        std::string BITstar::statesPrunedProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numFreeStatesPruned_);
        }



        std::string BITstar::verticesDisconnectedProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numVerticesDisconnected_);
        }



        std::string BITstar::rewiringProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numRewirings_);
        }



        std::string BITstar::stateCollisionCheckProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numStateCollisionChecks_);
        }



        std::string BITstar::edgeCollisionCheckProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numEdgeCollisionChecks_);
        }



        std::string BITstar::nearestNeighbourProgressProperty() const
        {
            return boost::lexical_cast<std::string>(numNearestNeighbours_);
        }
    }//geometric
}//ompl
