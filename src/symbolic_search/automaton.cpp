#include "automaton.h"
#include "sym_variables.h"
#include "transition_relation.h"
#include "cuddObj.hh"

#include <chrono>
#include <vector>
#include <string>
#include <map>
#include <stack>
#include <cassert>
#include <iostream>
#include <queue>
#include <fstream>
#include <unordered_set>


std::vector<string> vertex_names;
std::vector<int> vertex_to_method;
// adjacency matrix, map contains:
// 1. task on the edge
// 2. the target vertex
// 3. the cost
// 4. the abstract task round + 1   -    or 0 for the primitive round
// 4. the BDD
std::vector<std::map<int,std::map<int, std::map<int, std::map<int, BDD>>>>> edges; 

std::map<int,std::pair<int,int>> tasks_per_method; // first and second

std::string to_string(Model * htn){
	std::string s = "digraph graphname {\n";

	for (int v = 0; v < vertex_names.size(); v++)
		//s += "  v" + std::to_string(v) + "[label=\"" + vertex_names[v] + "\"];\n";
		s += "  v" + std::to_string(v) + "[label=\"" + std::to_string(vertex_to_method[v]) + "\"];\n";
	
	s+= "\n\n";

	for (int v = 0; v < vertex_names.size(); v++)
		for (auto & [task,tos] : edges[v])
			for (auto & [to,bdd] : tos)
				s += "  v" + std::to_string(v) + " -> v" + std::to_string(to) + " [label=\"" + 
					//htn->taskNames[task] + "\"]\n";
					std::to_string(task) + "\"]\n";

	return s + "}";
}


void graph_to_file(Model* htn, std::string file){
	std::ofstream out(file);
    out << to_string(htn);
    out.close();
}

void ensureBDD(int from, int task, int to, int cost, int abstractRound, symbolic::SymVariables & sym_vars){
	if (!edges[from].count(task) || !edges[from][task].count(to) || !edges[from][task][to].count(cost) || !edges[from][task][to][cost].count(abstractRound))
		edges[from][task][to][cost][abstractRound] = sym_vars.zeroBDD();

}
void ensureBDD(int task, int to, int cost, int abstractRound, symbolic::SymVariables & sym_vars){
	ensureBDD(0,task,to,cost, abstractRound, sym_vars);
}





std::vector<symbolic::TransitionRelation> trs;


//================================================== extract solution


typedef std::tuple<int,int,int,int,int,int,int> tracingInfo;

std::vector<std::pair<int,std::string>> primitivePlan;
std::vector<std::tuple<int,std::string,std::string,int,int>> abstractPlan;
std::map<int,int> stackTasks;

int blup = 0;

void extract(int curCost, int curDepth, int curTask, int curTo,
	std::stack<int> & taskStack,
	std::stack<int> & methodStack,
	BDD state,
	int method,
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> abst_q
		){
	if (curCost == -1){
		std::cout << "End of solution" << std::endl;

		// output plan
		std::cout << "==>" << std::endl;
		for (int i = primitivePlan.size() - 1; i >= 0; i--)
			std::cout << primitivePlan[i].first << " " << primitivePlan[i].second << std::endl;
		std::cout << "root " << primitivePlan.size() + abstractPlan.size() - 1 << std::endl;
		for (int i = abstractPlan.size() - 1; i >= 0; i--){
			auto abstractEntry = abstractPlan[i];
			std::cout << get<0>(abstractEntry) << " " << get<1>(abstractEntry) << " -> " << get<2>(abstractEntry);
			if (get<3>(abstractEntry) != -1) std::cout << " " << get<3>(abstractEntry);
			if (get<4>(abstractEntry) != -1) std::cout << " " << get<4>(abstractEntry);
			std::cout << std::endl;
		}
		std::cout << "<==" << std::endl;

		exit(0);
	}

	// state contains in v the current state and in v'' something (... ikn)
	BDD previousState; // compute this
	if (curTask < htn->numActions) {
		// do a pre-image
		previousState = trs[curTask].preimage(state);
	} else {
		// nothing? Maybe update the stack push state
		previousState = state;
	}

	// check whether we can actually come from here
	BDD possibleSourceState = previousState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux) * edges[0][curTask][curTo][curCost][curDepth];
	if (possibleSourceState.IsZero()) return;


	// add this thing to the tracing
	std::cout << "Extracting solution starting " << curCost << " " << curDepth << ": " << curTask << " " << vertex_to_method[curTo];
	if (curTask >= htn->numActions) std::cout << "\t\t\t\t";
	std::cout << "\t\t\t\t\t" << htn->taskNames[curTask] << std::endl;

	int myTaskID = primitivePlan.size() + abstractPlan.size();
	if (curTask < htn->numActions) {
		primitivePlan.push_back({myTaskID, htn->taskNames[curTask]});
		methodStack.push(curTo);
	} else {
		int a = -1;
		int b = -1;
		std::cout << "\t\tAbstract, stack size = " << taskStack.size() << " " << htn->numSubTasks[method] << std::endl;
		if (htn->numSubTasks[method] == 0){
			methodStack.push(curTo);
		} else if (htn->numSubTasks[method] == 1){
			if (curTo != methodStack.top()){ // not possible
				std::cout << "\t\tCan't go to " << vertex_to_method[curTo] << " on stack is " << vertex_to_method[methodStack.top()] << std::endl;
				return;
			}
			a = taskStack.top();
			taskStack.pop();
		} else if (htn->numSubTasks[method] == 2){ // something else cannot happen
			// pop the first one
			a = taskStack.top();
			taskStack.pop();
			
			b = taskStack.top();
			// check if we are on the correct path
			if (stackTasks[b] != tasks_per_method[method].second){
				std::cout << "\t\tFAIL " << tasks_per_method[method].second << " " << stackTasks[a] << " " << method << std::endl;
				taskStack.push(a);
				return; // we failed!
			}
			
			// method stack, I think it is irrelevant what the intermediate method is
			int mstack = methodStack.top(); methodStack.pop();
			
			if (curTo != methodStack.top()){ // not possible
				std::cout << "\t\tCan't go to " << vertex_to_method[curTo] << " on stack is " << vertex_to_method[methodStack.top()] << std::endl;
				methodStack.push(mstack);
				taskStack.push(a);
				return;
			}

			taskStack.pop();
		}
		abstractPlan.push_back({myTaskID, htn->taskNames[curTask], htn->methodNames[method], a, b});
	}
	taskStack.push(myTaskID);
	stackTasks[myTaskID] = curTask;
	std::cout << "\t\tAccept" << std::endl;



	// look at my sources
	std::tuple<int,int> tup = {curTask, curTo};
	auto & myPredecessors = (curDepth == 0) ? prim_q[curCost][tup] : abst_q[curCost][curDepth][tup];
	std::cout << "\t\tOptions: " << myPredecessors.size() << std::endl;
	for (auto & [preCost, preDepth, preTask, preTo, _method, cost, getHere] : myPredecessors)
		std::cout << "\t\t\tEdge " << " " << preTask << " " << vertex_to_method[preTo] << std::endl;
	
	
	//if (myPredecessors.size() > 1){
	//  	blup++;
	//	if (blup == 3){
	//		sym_vars.bdd_to_dot(possibleSourceState, "source.dot");
	//		int i = 0;
	//		for (auto & [preCost, preDepth, preTask, preTo, _method, cost, getHere] : myPredecessors){
	//			std::cout << "Edge " << i << " " << preTask << " " << vertex_to_method[preTo] << std::endl;
	//			
	//			sym_vars.bdd_to_dot(edges[0][preTask][preTo][preCost][preDepth], "edge" + std::to_string(i++) + ".dot");
	//		}

	//		exit(0);
	//	}
	//}
	
	//std::reverse(myPredecessors.begin(), myPredecessors.end());
	for (auto & [preCost, preDepth, preTask, preTo, _method, cost, getHere] : myPredecessors){
		if (cost != -1){
			std::cout << "Epsilontik-tracing cost=" << cost << " getHere=" << getHere << std::endl;
			
			
			exit(0);
		}


		extract(preCost, preDepth, preTask, preTo, taskStack, methodStack, possibleSourceState, _method, htn, sym_vars, prim_q, abst_q);
	}

	std::cout << "Backtracking failed at this point ... " << std::endl;
	exit(0);
}

void extract(int curCost, int curDepth, int curTask, int curTo,
	BDD state,
	int method, 
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> abst_q
		){
	std::stack<int> ss;
	std::stack<int> sm;
	extract(curCost, curDepth, curTask, curTo, ss, sm, state, method, htn, sym_vars, prim_q, abst_q);
	exit(0);
}


//================================================== planning algorithm

void build_automaton(Model * htn){

	// Symbolic Playground
	symbolic::SymVariables sym_vars(htn);
	sym_vars.init(true);
	BDD init = sym_vars.getStateBDD(htn->s0List, htn->s0Size);
	BDD goal = sym_vars.getPartialStateBDD(htn->gList, htn->gSize);
	
	for (int i = 0; i < htn->numActions; ++i) {
	  std::cout << "Creating TR " << i << std::endl;
	  trs.emplace_back(&sym_vars, i, htn->actionCosts[i]);
	  trs.back().init(htn);
	  //sym_vars.bdd_to_dot(trs.back().getBDD(), "op" + std::to_string(i) + ".dot");
	}



	// actual automaton construction
	std::vector<int> methods_with_two_tasks;
	// the vertex number of these methods, 0 and 1 are start and end vertex
	std::map<int,int> methods_with_two_tasks_vertex;
	vertex_to_method.push_back(-1);
	vertex_to_method.push_back(-2);
	for (int m = 0; m < htn->numMethods; m++)
		if (htn->numSubTasks[m] == 2){
			methods_with_two_tasks.push_back(m);
			methods_with_two_tasks_vertex[m] = methods_with_two_tasks.size() + 1;
			vertex_to_method.push_back(m);
		}


	int number_of_vertices = 2 + methods_with_two_tasks.size();
	edges.resize(number_of_vertices);

	// build the initial version of the graph
	vertex_names.push_back("start");
	vertex_names.push_back("end");
	for (auto & [method, vertex] : methods_with_two_tasks_vertex){
		vertex_names.push_back(htn->methodNames[method]);

		// which one is the second task of this method
		int first = htn->subTasks[method][0];
		int second = htn->subTasks[method][1];
		assert(htn->numOrderings[method] == 2);
		if (htn->ordering[method][0] == 1 && htn->ordering[method][1])
			std::swap(first,second);

		//edges[0][first][vertex] = sym_vars.oneBDD();
		//edges[0][first][vertex] = sym_vars.zeroBDD();
		//for (int i = 0; i < htn->numVars; i++)
		//	edges[0][first][vertex] *= sym_vars.auxBiimp(i); // v_i = v_i''
	  	//sym_vars.bdd_to_dot(edges[0][first][vertex], "m_biimp" + std::to_string(method) + ".dot");
		tasks_per_method[method] = {first, second};
	}


	// add the initial abstract task
	edges[0][htn->initialTask][1][0][0] = init;



	// apply transition rules
	
	// loop over the outgoing edges of 0, only to those rules can be applied

	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> prim_q;
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> abst_q;
	std::vector<std::map<int,std::map<int,BDD>>> eps;
	std::vector<std::map<int,std::map<int,std::vector<tracingInfo> >>> eps_inserted (number_of_vertices);
	std::vector<std::map<int,BDD>> state_expanded_at_cost;

	std::map<int,std::map<int,BDD>> _empty_eps;
	_empty_eps[0][0] = sym_vars.zeroBDD();
	for (int i = 0; i < number_of_vertices; i++) eps.push_back(_empty_eps);
	std::map<int,BDD> _empty_state;
	_empty_state[0] = sym_vars.zeroBDD();
	for (int i = 0; i < number_of_vertices; i++) state_expanded_at_cost.push_back(_empty_state);

#define put push_back
//#define put insert

	std::tuple<int,int> _tup = {htn->initialTask, 1};
	tracingInfo _from = {-1,-1,-1,-1,-1,-1,-1};
	abst_q[0][1][_tup].push_back(_from);

	// cost of current layer and whether we are in abstract or primitive mode
	int currentCost = 0;
	int currentDepthInAbstract = 0; // will be zero for the primitive round
	
	auto addQ = [&] (int task, int to, int extraCost, int fromTask, int fromTo, int method, int cost, int getHere) {
		std::tuple<int,int> tup = {task, to};
		tracingInfo from = {currentCost, currentDepthInAbstract, fromTask, fromTo, method, cost, getHere};
		
		if (task >= htn->numActions || htn->actionCosts[task] == 0){
			// abstract task or zero-cost action
			if (!extraCost)
				abst_q[currentCost][currentDepthInAbstract+1][tup].push_back(from);
			else
				abst_q[currentCost + extraCost][1][tup].push_back(from);
		} else {
			// primitive with cost
			prim_q[currentCost + extraCost + htn->actionCosts[task]][tup].push_back(from);
		}
	};



	std::clock_t layer_start = std::clock();
	int lastCost;
	int lastDepth;

	int step = 0;
	std::map<std::tuple<int,int>, std::vector<tracingInfo> > & current_queue = prim_q[0];
	// TODO separate trans and rel
	while (true){ // TODO: detect unsolvability
		for (auto & entry : current_queue){
			int task = get<0>(entry.first);
			int to   = get<1>(entry.first);
			//current_queue.pop_front();
			//int task = get<0>(*cq.begin());
			//int to   = get<1>(*cq.begin());
			//cq.erase(cq.begin());

			//ensureBDD(task, to, sym_vars); // necessary?
			BDD state = edges[0][task][to][lastCost][lastDepth];
			//sym_vars.bdd_to_dot(state, "state" + std::to_string(step) + ".dot");
	  
			//std::cout << "Got BDD" << std::endl;	

			//std::cout << "\t\t\t\t\ttask ID" << task << std::endl;
			//std::cout << "\t\t\t\t\tto" << to << std::endl;

			if (step % 10000 == 0){
				std::cout << "STEP #" << step << ": " << task << " " << vertex_to_method[to] << std::endl;
	   			std::cout << "\t\t" << htn->taskNames[task] << std::endl;		
			}
			
			if (state == sym_vars.zeroBDD()) continue; // impossible state, don't treat it

			if (task < htn->numActions){
				//std::cout << "Prim: " << htn->taskNames[task] << std::endl;
				// apply action to state
				BDD nextState = trs[task].image(state);
				nextState = nextState.SwapVariables(sym_vars.swapVarsEff, sym_vars.swapVarsAux);
				//sym_vars.bdd_to_dot(nextState, "nextState" + std::to_string(step) + ".dot");
				//sym_vars.bdd_to_dot(eps[to], "eps" + std::to_string(step) + ".dot");

				// check if already added
				// TODO: there is a bit faster code here
 				BDD disjunct = eps[to][currentCost][currentDepthInAbstract] + nextState;  
				if (disjunct != eps[to][currentCost][currentDepthInAbstract]){
					eps[to][currentCost][currentDepthInAbstract] = disjunct; // TODO: check logic here

					//std::cout << "LOOP" << std::endl;
				
					for (auto & [task2,tos] : edges[to])
						for (auto & [to2,bdds] : tos){
							if (!bdds.count(lastCost) || !bdds[lastCost].count(lastDepth)) continue;
							BDD bdd = bdds[lastCost][lastDepth];
							BDD addState = nextState.AndAbstract(bdd,sym_vars.existsVarsEff);

							
							ensureBDD(task2, to2, currentCost, currentDepthInAbstract, sym_vars);
							
							BDD edgeDisjunct = edges[0][task2][to2][currentCost][currentDepthInAbstract] + addState;
							if (edgeDisjunct != edges[0][task2][to2][currentCost][currentDepthInAbstract]){
						   		edges[0][task2][to2][currentCost][currentDepthInAbstract] = edgeDisjunct;
								//std::cout << "\tPrim: " << task2 << " " << vertex_to_method[to2] << std::endl;
								addQ(task2, to2, 0, task, to, -1, -1, -1); // no method as primitive
							} else {
								// not new but successors might be  ...
								//addQ(task2,to2,0);
								//std::cout << "\tKnown state: " << task2 << " " << vertex_to_method[to2] << std::endl;
							}
						}

					if (to == 1 && nextState * goal != sym_vars.zeroBDD()){
						std::cout << "Goal reached! Length=" << currentCost << " steps=" << step << std::endl;
	  					// sym_vars.bdd_to_dot(nextState, "goal.dot");
						extract(currentCost, currentDepthInAbstract, task, to, nextState * goal, -1, htn, sym_vars, prim_q, abst_q);
						return;
					}
				} else {
					//std::cout << "\tNot new for Eps" << std::endl;
				}
			} else {
				// abstract edge, go over all applicable methods	
				
				for(int mIndex = 0; mIndex < htn->numMethodsForTask[task]; mIndex++){
					int method = htn->taskToMethods[task][mIndex];
					//std::cout << "Method " << htn->methodNames[method] << std::endl;	
					//std::cout << "\t==Method " << method << std::endl;

					// cases
					if (htn->numSubTasks[method] == 0){

						BDD nextState = state;
						nextState = nextState.SwapVariables(sym_vars.swapVarsEff, sym_vars.swapVarsAux);

						// check if already added
						BDD disjunct = eps[to][currentCost][currentDepthInAbstract] + nextState;
						if (disjunct != eps[to][currentCost][currentDepthInAbstract]){
							eps[to][currentCost][currentDepthInAbstract] = disjunct;
	
							for (auto & [task2,tos] : edges[to])
								for (auto & [to2,bdds] : tos){
									if (!bdds.count(lastCost) || !bdds[lastCost].count(lastDepth)) continue;
									BDD bdd = bdds[lastCost][lastDepth];
									
									BDD addState = nextState.AndAbstract(bdd,sym_vars.existsVarsEff);
									
									ensureBDD(task2,to2,currentCost,currentDepthInAbstract,sym_vars);
									BDD edgeDisjunct = edges[0][task2][to2][currentCost][currentDepthInAbstract] + addState;
									if (edgeDisjunct != edges[0][task2][to2][currentCost][currentDepthInAbstract]){
								   		edges[0][task2][to2][currentCost][currentDepthInAbstract] = edgeDisjunct;
										//std::cout << "\tEmpty Method: " << task2 << " " << vertex_to_method[to2] << std::endl;
										addQ(task2, to2, 0, task, to, method, -1, -1);
									} else {
										//addQ(task2, to2, 0);
									}
								}
	
							if (to == 1 && nextState * goal != sym_vars.zeroBDD()){
								std::cout << "Goal reached! Length=" << currentCost << " steps=" << step <<  std::endl;
								extract(currentCost, currentDepthInAbstract, task, to, nextState * goal, method, htn, sym_vars, prim_q, abst_q);
								return;
							}
						}
					} else if (htn->numSubTasks[method] == 1){
						// ensure that a BDD is there
						ensureBDD(htn->subTasks[method][0], to, currentCost, currentDepthInAbstract, sym_vars);
						
						// perform the operation
						BDD disjunct = edges[0][htn->subTasks[method][0]][to][currentCost][currentDepthInAbstract] + state;
						if (disjunct != edges[0][htn->subTasks[method][0]][to][currentCost][currentDepthInAbstract]){
							edges[0][htn->subTasks[method][0]][to][currentCost][currentDepthInAbstract] = disjunct;
							//std::cout << "\tUnit: " << htn->subTasks[method][0] << " " << vertex_to_method[to] << std::endl;
							addQ(htn->subTasks[method][0], to, 0, task, to, method, -1, -1);
						}
						
					} else { // two subtasks
						assert(htn->numSubTasks[method] == 2);
						// add edge (state is irrelevant here!!)
						
						//std::cout << "\tOutgoing edge" << std::endl;
					
						BDD r_temp = state.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsEff);
					
						ensureBDD(methods_with_two_tasks_vertex[method], tasks_per_method[method].second, to, currentCost, currentDepthInAbstract, sym_vars);
					
						BDD disjunct_r_temp = edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract] + r_temp;
						if (disjunct_r_temp != edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract]){
							//addQ(tasks_per_method[method].first, methods_with_two_tasks_vertex[method]);
							edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract] = disjunct_r_temp;
						}
						
						//std::cout << "\tEpsilontik" << std::endl;
						for (int cost = 0; cost <= lastCost; cost++){
							BDD stateAtRoot = (--eps[methods_with_two_tasks_vertex[method]][cost].end()) -> second;
							BDD intersectState = stateAtRoot.And(state);
							
							if (!intersectState.IsZero()){
								//std::cout << "\t\t\tFound intersecting state cost " << cost << std::endl;
								// find the earliest time we could have gotten here
								for (int getHere = 0; getHere <= cost; getHere++){
									//std::cout << "\t\t\t\ttrying source " << getHere << std::endl;
									// could we have gotten here?
									BDD newState = intersectState.AndAbstract(
											state_expanded_at_cost[methods_with_two_tasks_vertex[method]][getHere],
											sym_vars.existsVarsEff);
									
									if (!newState.IsZero()){
										int actualCost = cost - getHere;
										//std::cout << "\t\tEpsilon with cost " << actualCost << std::endl;
										int targetCost = currentCost + actualCost;
									
										int targetDepth = 0;
										if (actualCost == 0)
											targetDepth = currentDepthInAbstract;
										
										ensureBDD(tasks_per_method[method].second,to,targetCost,targetDepth, sym_vars); // add as an edge to the future
										
										
										BDD disjunct = edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] + newState;
										if (edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] != disjunct){
											edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] = disjunct;
											//std::cout << "\t2 EPS: " << tasks_per_method[method].second << " " << vertex_to_method[to] << " cost " << actualCost << std::endl;
											//std::cout << "\tedge " << targetCost << " " << targetDepth << std::endl;
											addQ(tasks_per_method[method].second, to, actualCost, task, to, method, cost, getHere); // TODO think about when to add ... the depth in abstract should be irrelevant?
										}
										break;
									} else {
										//std::cout << "\t\t\t\t\tis zero" << std::endl;
									}
								}
							}
						}

						//std::cout << "\tIngoing edge" << std::endl;
						
						// new state for edge to method vertex
						ensureBDD(tasks_per_method[method].first, methods_with_two_tasks_vertex[method], currentCost, currentDepthInAbstract, sym_vars);
			
						BDD biimp = sym_vars.oneBDD();
						for (int i = 0; i < htn->numVars; i++)
							biimp *= sym_vars.auxBiimp(i); // v_i = v_i''

						BDD ss = state.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux);
						ss *= biimp;
						
						// memorise at which cost we got here, n the v_i' variables
						state_expanded_at_cost[methods_with_two_tasks_vertex[method]][currentCost] +=
							ss.SwapVariables(sym_vars.swapVarsEff, sym_vars.swapVarsPre);
			
						BDD disjunct2 = edges[0][tasks_per_method[method].first][methods_with_two_tasks_vertex[method]][currentCost][currentDepthInAbstract] + ss;
					   	if (true || disjunct2 != edges[0][tasks_per_method[method].first][methods_with_two_tasks_vertex[method]][currentCost][currentDepthInAbstract]){
						   edges[0][tasks_per_method[method].first][methods_with_two_tasks_vertex[method]][currentCost][currentDepthInAbstract] = disjunct2;
							
						   //std::cout << "\t2 normal: " << tasks_per_method[method].first << " " << vertex_to_method[methods_with_two_tasks_vertex[method]] << std::endl;
						   addQ(tasks_per_method[method].first, methods_with_two_tasks_vertex[method], 0, task, to, method, -1, -1);
					  }	
					}
				}
			}

			//graph_to_file(htn,"graph" + std::to_string(step++) + ".dot");
			step++;
		}

		// handled everything in this layer	
		// switch to next layer	
		std::clock_t layer_end = std::clock();
		double layer_time_in_ms = 1000.0 * (layer_end-layer_start) / CLOCKS_PER_SEC;
		std::cout << "Layer time: " << layer_time_in_ms << "ms" << std::endl << std::endl;
		layer_start = layer_end;

		lastCost = currentCost;
		lastDepth = currentDepthInAbstract;

		// check whether we have to stay abstract
		std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>::iterator next_abstract_layer;
		if (currentDepthInAbstract == 0){
			next_abstract_layer = abst_q[currentCost].begin();
		} else {
			next_abstract_layer = abst_q[currentCost].find(currentDepthInAbstract);
			if (next_abstract_layer != abst_q[currentCost].end()){
				next_abstract_layer++;
			}
		}
		
		if (next_abstract_layer == abst_q[currentCost].end()){
			// transition to primitive layer
			currentCost = (++prim_q.find(currentCost))->first;
			currentDepthInAbstract = 0; // the primitive layer
			current_queue = prim_q[currentCost];
			std::cout << "========================== Cost Layer " << currentCost << std::endl; 
		} else {
			currentDepthInAbstract = next_abstract_layer->first;
			current_queue = next_abstract_layer->second;
			std::cout << "========================== Abstract layer of cost " << currentCost << " layer: " << currentDepthInAbstract << std::endl; 
		}

		//std::cout << "COPY " << std::endl; 

		// copy the graph for the new cost layer
		for (int from = 0; from < edges.size(); from++)
			for (auto & [task,es] : edges[from])
				for (auto & [to, bdds] : es){
					if (!bdds.count(lastCost)) continue;
					if (!bdds[lastCost].count(lastDepth)) continue;
					ensureBDD(from,task,to,currentCost,currentDepthInAbstract,sym_vars); // might already contain a BDD from epsilon transitions
					edges[from][task][to][currentCost][currentDepthInAbstract] += 
						bdds[lastCost][lastDepth];
				}
		
		//std::cout << "edges done" << std::endl; 
		
		// epsilon set
		for (int from = 0; from < edges.size(); from++)
			eps[from][currentCost][currentDepthInAbstract] = eps[from][lastCost][lastDepth];
		
		//std::cout << "eps done" << std::endl; 
		
		if (currentCost != lastCost)
			for (int from = 0; from < edges.size(); from++){
				state_expanded_at_cost[from][currentCost] = state_expanded_at_cost[from][lastCost];
			}
		
		//std::cout << "state done" << std::endl; 
	}
	
	std::cout << "Ending ..." << std::endl;
	exit(0);
	//delete sym_vars.manager.release();






	//std::cout << to_string(htn) << std::endl;
}

