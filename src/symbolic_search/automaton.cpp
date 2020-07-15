#include "../debug.h"
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
				s += "  v" + std::to_string(v) + " -> v" + std::to_string(to) + " [label=\"" + std::to_string(task) + "\"]\n";
	
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

void printStack(std::deque<int> & taskStack, std::deque<int> & methodStack, int indent){
	for (size_t i = 0; i < taskStack.size(); i++){
		for (int j = 0; j < indent; j++) std::cout << "\t";
		std::cout << "\t\t#" << i << " " << taskStack[i] << " " << vertex_to_method[methodStack[i]] << std::endl;
	}
}


int global_id_counter = 0;

struct reconstructed_plan {
	bool success;

	int root;
	
	std::deque<std::pair<int,int>> primitive_plan;
	std::deque<std::tuple<int,int,int,int,int>> abstract_plan;
	
	
	std::deque<int> currentStack;


	void printPlan(Model * htn){
		if (currentStack.size()){
			std::cout << "Stack:";
			for (int x : currentStack) std::cout << " " << x;
			std::cout << std::endl;
		}
		std::cout << "==>" << std::endl;
		for (std::pair<int,int> p : primitive_plan)
			std::cout << p.first << " " << htn->taskNames[p.second] << std::endl;
	
		std::cout << "root " << root << std::endl;
		
		for (auto & [id,task,method,task1,task2] : abstract_plan){
			std::cout << id << " " << htn->taskNames[task] << " -> " << htn->methodNames[method];
			if (task1 != -1) std::cout << " " << task1;
			if (task2 != -1) std::cout << " " << task2;
			std::cout << std::endl;
		}

		std::cout << "<==" << std::endl;
	}
};


reconstructed_plan get_fail(){
	reconstructed_plan r;
	r.success = false;
	return r;
}

reconstructed_plan get_empty_success(){
	reconstructed_plan r;
	r.success = true;
	r.root = global_id_counter;
	r.currentStack.push_front(global_id_counter++);
	return r;
}



// forward declaration
reconstructed_plan extract2(int curCost, int curDepth, int curTask, int curTo,
	int targetTask,
	std::deque<int>  taskStack,
	std::deque<int>  methodStack,
	BDD & curState,
	int method,
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> & prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> & abst_q,
	std::vector<std::map<int,std::vector<tracingInfo> >> & eps_inserted
		);


reconstructed_plan extract2From(int curCost, int curDepth, int curTask, int curTo,
	int targetTask,
	std::deque<int>  taskStack,
	std::deque<int>  methodStack,
	BDD & curState,
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> & prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> & abst_q,
	std::vector<std::map<int,std::vector<tracingInfo> >> & eps_inserted
		){
	
	assert(taskStack.size() == methodStack.size());
	
	DEBUG(std::cout << "\t" << color(YELLOW,"Extract from ") << curCost << " " << curDepth << ": " << curTask << " " << vertex_to_method[curTo] << " stack size: " << taskStack.size() << " target " << targetTask << std::endl);
	if (targetTask != -1 && taskStack.size() == 1 && taskStack[0] == targetTask){
		DEBUG(std::cout << "\t\t" << color(GREEN,"Got to target task on stack.") << std::endl);
		return get_empty_success();
	}
	
	// look at my sources
	std::tuple<int,int> tup = {curTask, curTo};
	auto & myPredecessors = (curDepth == 0) ? prim_q[curCost][tup] : abst_q[curCost][curDepth][tup];
	DEBUG(
			std::cout << "\tOptions: " << myPredecessors.size() << std::endl;
			for (auto & [preCost, preDepth, preTask, preTo, _method, cost, getHere] : myPredecessors)
				std::cout << "\t\tEdge " << " " << preTask << " " << vertex_to_method[preTo] << std::endl;
		);
	
	for (auto & [preCost, preDepth, preTask, preTo, _method, cost, getHere] : myPredecessors){
		DEBUG(std::cout << "\t\t" << color(YELLOW, "Trying Edge ") << preCost << " " << preDepth << " " << preTask << " " << vertex_to_method[preTo] << std::endl);
		
		if (cost != -1){
			DEBUG(std::cout << color(YELLOW,"\t\tThis edge was inserted due to an epsilon application.") << " cost=" << cost << " getHere=" << getHere << " " << vertex_to_method[getHere] << std::endl);
			
			int appliedMethod = vertex_to_method[getHere];
			int abstractTask = htn->decomposedTask[appliedMethod];
			DEBUG(std::cout << "\t\t\tTask on the main edge is " << htn->taskNames[abstractTask] << " (#" << abstractTask << ")" << std::endl); 
			for (auto & [preCost2, preDepth2, preTask2, preTo2, _method2, remainingTask, remainingMethod] : eps_inserted[getHere][cost]){
				DEBUG(std::cout << "\t\t\t" << preCost2 << " " << preDepth2 << " " << preTask2 << " " << preTo2 << " " << _method2 << " " << remainingTask << " " << remainingMethod << " " << endl);
			
				// start reconstruction with empty stack	
				std::deque<int> ss;
				std::deque<int> sm;

				ss.push_back(remainingTask);
				sm.push_back(remainingMethod);

				reconstructed_plan a = extract2(preCost2, preDepth2, preTask2, preTo2, abstractTask, ss, sm, curState, _method2, htn, sym_vars, prim_q, abst_q, eps_inserted);
				
				if (a.success){
					// apply the method
					
					ss = taskStack;
					sm = methodStack;
					
					ss.pop_front();
					ss.push_front(abstractTask);
				
					DEBUG(printStack(ss,sm,8));
					assert(ss.front() == preTask);
	
					reconstructed_plan b = extract2From(preCost, preDepth, ss.front(), sm.front(), targetTask,
							ss, sm, curState, htn, sym_vars, prim_q, abst_q, eps_inserted);
					
					if (b.success) {
						//a.printPlan(htn);
						//b.printPlan(htn);
						
						for (auto x : b.primitive_plan)
							a.primitive_plan.push_back(x);
						for (auto x : b.abstract_plan)
							a.abstract_plan.push_back(x);
						
						// the one decomposition we do here
	
						assert(a.currentStack.size() == 1);
						assert(b.currentStack.size() >= 1);
						int aRoot = a.root;
						int aStackTop = a.currentStack.front();
						int bStackTop = b.currentStack.front();
						// replace 
						for (auto & x : a.primitive_plan)
							if (get<0>(x) == aRoot)
								get<0>(x) = bStackTop;
						for (auto & x : a.abstract_plan)
							if (get<0>(x) == aRoot)
								get<0>(x) = bStackTop;
	
	
						a.currentStack = b.currentStack;
						a.currentStack.pop_front();
						a.currentStack.push_front(aStackTop);
						
						a.root = b.root;
					
						//a.printPlan(htn);
	
						return a;
					}
				}
			}
			continue;
		}

		if (preCost == curCost){
			// the current layer is an abstract one. Then going to the previous layer internal parts of the stack may vanish.
			BDD state = curState; // stack state, note that this is not the state we are currently in, but the one that is used to connect valid stacks
			int currentVertex = 0;
			int lastUnmatchingStack = -1;
			for (size_t i = 0; i < methodStack.size(); i++){
				DEBUG(std::cout << "\t\t\t\t\t\t\t\t\tStack (2) pos " << i << "/" << methodStack.size() << std::endl);
				int taskToGo = taskStack[i];
				int targetVertex = methodStack[i];
				
				ensureBDD(currentVertex,taskToGo,targetVertex,curCost,curDepth,sym_vars); // maybe there isn't even a BDD
				BDD transitionBDD = edges[currentVertex][taskToGo][targetVertex][curCost][curDepth];
				BDD nextBDD;
				if (i != 0) // state is in v'
					state = state.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsEff);
				
				nextBDD = transitionBDD * state; // v'' contains state after the edge
				
				if (i != 0){
					ensureBDD(currentVertex,taskToGo,targetVertex,curCost,curDepth-1,sym_vars); // maybe there isn't even a BDD
					BDD presentInPrevious = edges[currentVertex][taskToGo][targetVertex][curCost][curDepth-1] * nextBDD;
					if (presentInPrevious.IsZero()){
						DEBUG(std::cout << "\t\t\t" << color(RED,"stack element not present in previous round ") << vertex_to_method[currentVertex] << " " << 
							taskToGo << " " << vertex_to_method[targetVertex] << std::endl);
						lastUnmatchingStack = i;	
					}
				}
				
				nextBDD = nextBDD.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsAux);
				nextBDD = nextBDD.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux);
	
				state = nextBDD;
				currentVertex = targetVertex;
			}

			if (lastUnmatchingStack != -1){
				// split the stack in two and call separately
				DEBUG(printStack(taskStack, methodStack, 2));
				DEBUG(std::cout << color(YELLOW,"\t\tStack does not match") << ", splitting at position " << lastUnmatchingStack << std::endl);
				int appliedMethod = vertex_to_method[methodStack[lastUnmatchingStack-1]]; 
				DEBUG(std::cout << "\t\t\tApplied method is " << htn->methodNames[appliedMethod] << " (#" << appliedMethod << ")" << std::endl);

				std::deque<int> firstTaskStack;
				std::deque<int> firstMethodStack;
				std::deque<int> secondTaskStack;
				std::deque<int> secondMethodStack;
				secondTaskStack.push_back(htn->decomposedTask[appliedMethod]);
				secondMethodStack.push_back(methodStack[lastUnmatchingStack]);

				for (size_t i = 0; i < methodStack.size(); i++){
					if (i < lastUnmatchingStack){
						firstTaskStack.push_back(taskStack[i]);
						firstMethodStack.push_back(methodStack[i]);
					} else if (i > lastUnmatchingStack){
						secondTaskStack.push_back(taskStack[i]);
						secondMethodStack.push_back(methodStack[i]);
					}
				}
				
				DEBUG(
					std::cout << "\t\t\tStack 1:" << std::endl;
					printStack(firstTaskStack, firstMethodStack, 2);
					std::cout << "\t\t\tStack 2:" << std::endl;
					printStack(secondTaskStack, secondMethodStack, 2);
					std::cout << "\t\t\tHead task of method " << tasks_per_method[appliedMethod].first << std::endl;
					);
				
				
				reconstructed_plan a = extract2From(curCost, curDepth, firstTaskStack.front(), firstMethodStack.front(), tasks_per_method[appliedMethod].first,
						firstTaskStack, firstMethodStack, curState, htn, sym_vars, prim_q, abst_q, eps_inserted);
				
				if (a.success) {
					reconstructed_plan b = extract2From(curCost, curDepth, secondTaskStack.front(), secondMethodStack.front(), targetTask,
							secondTaskStack, secondMethodStack, curState, htn, sym_vars, prim_q, abst_q, eps_inserted);
				
					if (b.success) {
						for (auto x : b.primitive_plan)
							a.primitive_plan.push_back(x);
						for (auto x : b.abstract_plan)
							a.abstract_plan.push_back(x);
						
						// the one decomposition we do here
						int taskToDecompose = b.currentStack.front(); b.currentStack.pop_front();
						int sub2 = global_id_counter++;
						a.abstract_plan.push_back({taskToDecompose, htn->decomposedTask[appliedMethod], appliedMethod, a.root, sub2});
						
						a.currentStack.push_back(sub2);
						for (auto x : b.currentStack)
							a.currentStack.push_back(x);
						
						a.root = b.root;
						return a;
					}
				}
				continue; // can't use this else ..
			}
		}

		reconstructed_plan a = extract2(preCost, preDepth, preTask, preTo, targetTask, taskStack, methodStack, curState, _method, htn, sym_vars, prim_q, abst_q, eps_inserted);
		if (a.success) return a;
	}

	DEBUG(std::cout << color(YELLOW,"Backtracking failed at this point ... ") << std::endl);
	return get_fail();
}


////////////// CORRECTS THE STACK TO FIT TO CURRENT LAYER
reconstructed_plan extract2(int curCost, int curDepth, int curTask, int curTo,
	int targetTask,
	std::deque<int>  taskStack,
	std::deque<int>  methodStack,
	BDD & curState,
	int method,
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> & prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> & abst_q,
	std::vector<std::map<int,std::vector<tracingInfo> >> & eps_inserted
		){
	assert(taskStack.size() == methodStack.size());
	
	if (curCost == -1){
		DEBUG(std::cout << "DONE" << std::endl);
		return get_empty_success();
	}
	
	DEBUG(
		std::cout << "Extracting solution starting cost=" << curCost << " depth=" << curDepth << " t=" << curTask << " m=" << vertex_to_method[curTo] << std::endl;
		if (curTask < htn->numActions) std::cout << "\tPRIM " << color(GREEN,htn->taskNames[curTask]) << std::endl;
		else                           std::cout << "\tABST " << color(BLUE,htn->taskNames[curTask]) << " method " << color(CYAN, htn->methodNames[method]) << 
													" #" << method << std::endl;
		);

	// output the stack
	assert(taskStack.size() == methodStack.size());



	///////////////////////////////////////// can we do this edge backwards?
	// state contains in v the current state and in v'' something
	BDD previousState; // compute this
	if (curTask < htn->numActions) {
		// do a pre-image
		previousState = trs[curTask].preimage(curState);
	} else {
		// abstract, state does not change
		previousState = curState;
	}

	// check whether we can actually come from here
	BDD possibleSourceState = previousState * edges[0][curTask][curTo][curCost][curDepth];
	possibleSourceState = possibleSourceState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsEff); // remove the effect variables
	possibleSourceState = possibleSourceState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux); // remove the auxiliary variables
	// possible source state is in v, this is the state at vertex 0


	if (possibleSourceState.IsZero()) {
		DEBUG(std::cout << color(RED,"\t\tBacktracking, state does not fit.") << std::endl);
		return get_fail();
	}

	// reconstruct how the stack would look like if we did this push
	
	if (curTask < htn->numActions) {
		// true action
		if (false && methodStack.size()) { // we can't do this check for the first edge
			// check whether we could have gone these two edges
			BDD edge1BDD = possibleSourceState.SwapVariables(sym_vars.swapVarsEff, sym_vars.swapVarsAux);
			BDD edge2BDD = edges[curTo][taskStack.front()][methodStack.front()][curCost][curDepth];

			possibleSourceState = edge1BDD * edge2BDD;
			possibleSourceState = possibleSourceState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsEff);
		
			if (possibleSourceState.IsZero()){	
				DEBUG(std:: cout << color(RED,"\tTransition from new edge to previous one is not possible.") << std::endl);
				return get_fail();
			}
		}

		// push the state onto the stack
		methodStack.push_front(curTo);
		taskStack.push_front(curTask);
	} else {
		if (htn->numSubTasks[method] == 0){
			methodStack.push_front(curTo);
			taskStack.push_front(curTask);
		} else if (htn->numSubTasks[method] == 1){
			if (taskStack[0] != htn->subTasks[method][0]){
				DEBUG(std:: cout << color(RED,"\tFirst task on stack does not match method.") << std::endl);
				return get_fail();
			}

			// state is already checked above
			
			taskStack.pop_front();		
			taskStack.push_front(curTask);
		} else if (htn->numSubTasks[method] == 2){ // something else cannot happen
			DEBUG(std::cout << "\tMethod expecting " << tasks_per_method[method].first << " " << tasks_per_method[method].second << " M " << vertex_to_method[curTo] << std::endl);
			
			if (taskStack[0] != tasks_per_method[method].first){
				DEBUG(std:: cout << color(RED,"\tFirst task on stack does not match method.") << std::endl);
				return get_fail();
			}
		
			int tasksToCleanup = 1;
			
			// check whether this method actually does to the vertex we are expecting
			if (taskStack[1] != tasks_per_method[method].second || curTo != methodStack[1]){
				DEBUG(
					if (taskStack[1] != tasks_per_method[method].second)
						std::cout << color(RED,"\tSecond task on stack does not match method.") << std::endl;
					else 
						std::cout << color(RED,"\tCan't go to ") << vertex_to_method[curTo] << " on stack is " << vertex_to_method[methodStack[1]] << std::endl;
					);
				
				DEBUG(std::cout << color(RED,"\tStack cleanup impossible.") << std::endl);
				return get_fail();
			}
			

			// check whether this edge is compatible with the states as we know them
			if (false){
				// check whether we could have gone these two edges
				BDD edge1BDD = possibleSourceState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsEff).SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsAux);
				BDD edge2BDD = edges[0][curTask][curTo][curCost][curDepth];

				possibleSourceState = edge1BDD * edge2BDD;
				possibleSourceState = possibleSourceState.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsEff);
				
				DEBUG(cout << "\t\t\t\t##################### 2 " << possibleSourceState.IsZero() << endl);
				
				if (possibleSourceState.IsZero()){
					return get_fail();
				}
			}
		
			taskStack.pop_front();
			
			for (int i = 0; i < tasksToCleanup; i++){
				taskStack.pop_front();
				methodStack.pop_front();
			}
			
			taskStack.push_front(curTask);
		}
	}
	
	DEBUG(
		std::cout << "\tStack (after modification): " << taskStack.size() << std::endl;
		printStack(taskStack,methodStack,0)
	);

	// check at this point whether the stack can actually look as we constructed
	BDD state = previousState; // stack state, note that this is not the state we are currently in, but the one that is used to connect valid stacks
	int currentVertex = 0;
	for (size_t i = 0; i < methodStack.size(); i++){
		DEBUG(std::cout << "\t\t\t\t\t\t\tStack pos " << i << "/" << methodStack.size() << std::endl); 
		int taskToGo = taskStack[i];
		int targetVertex = methodStack[i];
		ensureBDD(currentVertex,taskToGo,targetVertex,curCost,curDepth,sym_vars);
		BDD transitionBDD = edges[currentVertex][taskToGo][targetVertex][curCost][curDepth];
		BDD nextBDD;
		if (i != 0) // state is in v'
			state = state.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsEff);
		
		nextBDD = transitionBDD * state; // v'' contains state after the edge
		nextBDD = nextBDD.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsAux);
		nextBDD = nextBDD.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux);

		state = nextBDD;
		currentVertex = targetVertex;
		if (state.IsZero()){
			DEBUG(std::cout << color(RED,"\t\tNot a valid stack state.") << std::endl);
			return get_fail();
		}
	}

	// TODO: maybe the stack tracing has ruled out some of the states????



	reconstructed_plan a = extract2From(curCost, curDepth, curTask, curTo, targetTask, taskStack, methodStack, possibleSourceState, htn, sym_vars, prim_q, abst_q, eps_inserted);


	if (curTask < htn->numActions) {
		int stackTask = a.currentStack.front(); a.currentStack.pop_front();
		// add a primitive action
		a.primitive_plan.push_back({stackTask,curTask});
	} else {
		if (htn->numSubTasks[method] == 1){
			int stackTask = a.currentStack.front(); a.currentStack.pop_front();
			int sub1 = global_id_counter++;

			a.abstract_plan.push_back({stackTask, curTask, method, sub1, -1});
			a.currentStack.push_front(sub1);
		} else if (htn->numSubTasks[method] == 2){ // something else cannot happen
			int stackTask = a.currentStack.front(); a.currentStack.pop_front();
			int sub1 = global_id_counter++;
			int sub2 = global_id_counter++;

			a.abstract_plan.push_back({stackTask, curTask, method, sub1, sub2});
			a.currentStack.push_front(sub2);
			a.currentStack.push_front(sub1);
		} else {
			int stackTask = a.currentStack.front(); a.currentStack.pop_front();
			a.abstract_plan.push_back({stackTask, curTask, method, -1, -1});
		}
	}

	return a;
}



void extract(int curCost, int curDepth, int curTask, int curTo,
	BDD state,
	int method, 
	Model * htn,
	symbolic::SymVariables & sym_vars,
	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> & prim_q,
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> & abst_q,
	std::vector<std::map<int,std::vector<tracingInfo> >> & eps_inserted
	){
	std::cout << std::endl;
	std::cout << std::endl;
	std::cout << std::endl;
	std::cout << std::endl;
	std::cout << std::endl;
	std::cout << std::endl;
	std::cout << "========================================================================" << std::endl;

	std::deque<int> ss;
	std::deque<int> sm;

	state = state.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsEff); // remove the effect variables
	state = state.AndAbstract(sym_vars.oneBDD(), sym_vars.existsVarsAux); // remove the auxiliary variables
	
	
	std::clock_t plan_start = std::clock();
	reconstructed_plan a = extract2(curCost, curDepth, curTask, curTo, -1, ss, sm, state, method, htn, sym_vars, prim_q, abst_q, eps_inserted);
	std::clock_t plan_end = std::clock();
	double plan_time_in_ms = 1000.0 * (plan_end-plan_start) / CLOCKS_PER_SEC;
	std::cout << "Plan reconstruction time: " << fixed << plan_time_in_ms  << "ms " << fixed << plan_time_in_ms/1000 << "s " << fixed << plan_time_in_ms / 60000 << "min" << std::endl << std::endl;
	
	/// output the found plan
	if (a.success) std::cout << color(GREEN,"Extracted plan") << std::endl;
	else           std::cout << color(RED,  "Plan extraction failed") << std::endl;
	
	if (a.success) a.printPlan(htn);
	exit(0);
}


//================================================== planning algorithm

void build_automaton(Model * htn){
	std::clock_t preparation_start = std::clock();
	std::cout << "Building transition relations" << std::endl;
	
	// Symbolic Playground
	symbolic::SymVariables sym_vars(htn);
	sym_vars.init(true);
	BDD init = sym_vars.getStateBDD(htn->s0List, htn->s0Size);
	BDD goal = sym_vars.getPartialStateBDD(htn->gList, htn->gSize);
	
	for (int i = 0; i < htn->numActions; ++i) {
	  DEBUG(std::cout << "Creating TR " << i << std::endl);
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

		tasks_per_method[method] = {first, second};
	}


	// add the initial abstract task
	edges[0][htn->initialTask][1][0][0] = init;



	// apply transition rules
	
	// loop over the outgoing edges of 0, only to those rules can be applied

	std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >> prim_q;
	std::map<int,std::map<int,std::map<std::tuple<int,int>, std::vector<tracingInfo> >>> abst_q;
	std::vector<std::map<int,std::map<int,BDD>>> eps;
	std::vector<std::map<int,std::vector<tracingInfo> >> eps_inserted (number_of_vertices);
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
		
	std::clock_t preparation_end = std::clock();
	double preparation_time_in_ms = 1000.0 * (preparation_end-preparation_start) / CLOCKS_PER_SEC;
	std::cout << "Preparation time: " << fixed << preparation_time_in_ms << "ms " << fixed << preparation_time_in_ms/1000 << "s " << fixed << preparation_time_in_ms / 60000 << "min" << std::endl << std::endl;

	// cost of current layer and whether we are in abstract or primitive mode
	int currentCost = 0;
	int currentDepthInAbstract = 0; // will be zero for the primitive round
	
	auto addQ = [&] (int task, int to, int extraCost, int fromTask, int fromTo, int method, int cost, int getHere, bool insertOnlyIfNonEmpty) {
		std::tuple<int,int> tup = {task, to};
		tracingInfo from = {currentCost, currentDepthInAbstract, fromTask, fromTo, method, cost, getHere};

		std::vector<tracingInfo> * insertQueue;
		if (task >= htn->numActions || htn->actionCosts[task] == 0){
			// abstract task or zero-cost action
			if (!extraCost)
				insertQueue = &(abst_q[currentCost][currentDepthInAbstract+1][tup]);
			else
				insertQueue = &(abst_q[currentCost + extraCost][1][tup]);
		} else {
			// primitive with cost
			insertQueue = &(prim_q[currentCost + extraCost + htn->actionCosts[task]][tup]);
		}

		if (!insertOnlyIfNonEmpty || insertQueue->size() || (task < htn->numActions && htn->actionCosts[task] != 0)){
			DEBUG(std::cout << "\t\t\t\t\t\t" << color(GREEN,"Inserting ") << task << " " << vertex_to_method[to] << std::endl);
			insertQueue->push_back(from);
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
			if (entry.second.size() == 0) continue;
			int task = get<0>(entry.first);
			int to   = get<1>(entry.first);
		

			BDD state = edges[0][task][to][lastCost][lastDepth];
			//sym_vars.bdd_to_dot(state, "state" + std::to_string(step) + ".dot");
	  

#ifndef NDEBUG
			if (step % 1 == 0){
#else
			if (step % 10000 == 0){
#endif
				std::cout << color(BLUE,"STEP") << " #" << step << ": " << task << " " << vertex_to_method[to];
	   			std::cout << "\t\tTask: " << htn->taskNames[task] << std::endl;		
			}
			
			if (state == sym_vars.zeroBDD()) continue; // impossible state, don't treat it

			if (task < htn->numActions){
				DEBUG(std::cout << "Prim: " << htn->taskNames[task] << std::endl);
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
					

					for (auto & [task2,tos] : edges[to])
						for (auto & [to2,bdds] : tos){
							if (!bdds.count(lastCost) || !bdds[lastCost].count(lastDepth)) continue;
							BDD bdd = bdds[lastCost][lastDepth];
							BDD addState = nextState.AndAbstract(bdd,sym_vars.existsVarsEff);

							
							ensureBDD(task2, to2, currentCost, currentDepthInAbstract, sym_vars);
							
							BDD edgeDisjunct = edges[0][task2][to2][currentCost][currentDepthInAbstract] + addState;
							if (edgeDisjunct != edges[0][task2][to2][currentCost][currentDepthInAbstract]){
						   		edges[0][task2][to2][currentCost][currentDepthInAbstract] = edgeDisjunct;
								DEBUG(std::cout << "\tPrim: " << task2 << " " << vertex_to_method[to2] << std::endl);
								addQ(task2, to2, 0, task, to, -1, -1, -1, false); // no method as primitive
					
								
								tracingInfo tracingInf = {currentCost, currentDepthInAbstract, task, to, -1, task2, to2};
								eps_inserted[to][currentCost].push_back(tracingInf);
							} else {
								// not new but successors might be  ...
								//addQ(task2,to2,0);
								//std::cout << "\tKnown state: " << task2 << " " << vertex_to_method[to2] << std::endl;
							}
						}

					if (to == 1 && nextState * goal != sym_vars.zeroBDD()){
						std::cout << "Goal reached! Length=" << currentCost << " steps=" << step << std::endl;
	  					// sym_vars.bdd_to_dot(nextState, "goal.dot");
						
						std::clock_t search_end = std::clock();
						double search_time_in_ms = 1000.0 * (search_end-preparation_end) / CLOCKS_PER_SEC;
						std::cout << "Search time: " << fixed << search_time_in_ms << "ms " << fixed << search_time_in_ms / 1000 << "s " << fixed << search_time_in_ms / 60000 << "min" << std::endl << std::endl;
						
						extract(currentCost, currentDepthInAbstract, task, to, nextState * goal, -1, htn, sym_vars, prim_q, abst_q, eps_inserted);
						return;
					}
				} else {
					//std::cout << "\tNot new for Eps" << std::endl;
				}
			} else {
				// abstract edge, go over all applicable methods	
				
				for(int mIndex = 0; mIndex < htn->numMethodsForTask[task]; mIndex++){
					int method = htn->taskToMethods[task][mIndex];
					DEBUG(std::cout << "\t" << color(MAGENTA,"Method ") << htn->methodNames[method] << " (#" << method << ")" << std::endl);

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
										DEBUG(std::cout << "\tEmpty Method: " << task2 << " " << vertex_to_method[to2] << std::endl);
										addQ(task2, to2, 0, task, to, method, -1, -1, false);
									
										tracingInfo tracingInf = {currentCost, currentDepthInAbstract, task, to, -1, task2, to2};
										eps_inserted[to][currentCost].push_back(tracingInf);
									} else {
										//addQ(task2, to2, 0);
									}
								}
	
							if (to == 1 && nextState * goal != sym_vars.zeroBDD()){
								std::cout << "Goal reached! Length=" << currentCost << " steps=" << step <<  std::endl;
						
								std::clock_t search_end = std::clock();
								double search_time_in_ms = 1000.0 * (search_end-preparation_end) / CLOCKS_PER_SEC;
								std::cout << "Search time: " << fixed << search_time_in_ms << "ms " << fixed << search_time_in_ms / 1000 << "s " << fixed << search_time_in_ms / 60000 << "min" << std::endl << std::endl;
						
								extract(currentCost, currentDepthInAbstract, task, to, nextState * goal, method, htn, sym_vars, prim_q, abst_q, eps_inserted);
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
							DEBUG(std::cout << "\t\t" << color(GREEN,"Unit: ") << htn->subTasks[method][0] << " " << vertex_to_method[to] << std::endl);
							addQ(htn->subTasks[method][0], to, 0, task, to, method, -1, -1, false);
						}
						
					} else { // two subtasks
						assert(htn->numSubTasks[method] == 2);
						// add edge (state is irrelevant here!!)
						
						DEBUG(std::cout << "\t\ttwo element method into " << tasks_per_method[method].first << " " << tasks_per_method[method].second << std::endl);
					
						BDD r_temp = state.SwapVariables(sym_vars.swapVarsPre, sym_vars.swapVarsEff);
					
						ensureBDD(methods_with_two_tasks_vertex[method], tasks_per_method[method].second, to, currentCost, currentDepthInAbstract, sym_vars);
					
						BDD disjunct_r_temp = edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract] + r_temp;
						if (disjunct_r_temp != edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract]){
							// add the internal outgoing edge, if it is new
							edges[methods_with_two_tasks_vertex[method]][tasks_per_method[method].second][to][currentCost][currentDepthInAbstract] = disjunct_r_temp;

							DEBUG(std::cout << "\t\t" << color(BLUE, "Internal Edge ") << vertex_to_method[methods_with_two_tasks_vertex[method]] << " " << tasks_per_method[method].second << " " << vertex_to_method[to] << std::endl);
						}
						
						DEBUG(std::cout << "\t\t" << color(CYAN,"Epsilontik") << std::endl);
						for (int cost = 0; cost <= lastCost; cost++){
							BDD stateAtRoot = (--eps[methods_with_two_tasks_vertex[method]][cost].end()) -> second;
							BDD intersectState = stateAtRoot.And(state);
						
							if (eps_inserted[methods_with_two_tasks_vertex[method]][cost].size() == 0) continue; // there is no reason to consider this

							if (!intersectState.IsZero()){
								DEBUG(std::cout << "\t\t\tFound intersecting state cost " << cost << std::endl);
								// find the earliest time we could have gotten here
								for (int getHere = 0; getHere <= cost; getHere++){
									DEBUG(std::cout << "\t\t\t\ttrying source " << getHere << std::endl);
									// could we have gotten here?
									BDD newState = intersectState.AndAbstract(
											state_expanded_at_cost[methods_with_two_tasks_vertex[method]][getHere],
											sym_vars.existsVarsEff);
									
									if (!newState.IsZero()){
										int actualCost = cost - getHere;
										DEBUG(std::cout << "\t\t\tEpsilon with cost " << actualCost << std::endl);
										int targetCost = currentCost + actualCost;
									
										int targetDepth = 0;
										if (actualCost == 0)
											targetDepth = currentDepthInAbstract;
										
										ensureBDD(tasks_per_method[method].second,to,targetCost,targetDepth, sym_vars); // add as an edge to the future
										
										
										BDD disjunct = edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] + newState;
										if (edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] != disjunct){
											edges[0][tasks_per_method[method].second][to][targetCost][targetDepth] = disjunct;
											DEBUG(std::cout << "\t\t\t2 EPS: " << tasks_per_method[method].second << " " << vertex_to_method[to] << " cost " << actualCost << std::endl);
											DEBUG(std::cout << "\t\t" << color(GREEN,"edge ") << targetCost << " " << targetDepth << std::endl);
											addQ(tasks_per_method[method].second, to, actualCost, task, to, method, cost, methods_with_two_tasks_vertex[method], false); // TODO think about when to add ... the depth in abstract should be irrelevant?
										}
										break;
									} else {
										//std::cout << "\t\t\t\t\tis zero" << std::endl;
									}
								}
							}
						}

						
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
					   	if (disjunct2 != edges[0][tasks_per_method[method].first][methods_with_two_tasks_vertex[method]][currentCost][currentDepthInAbstract]){
						   edges[0][tasks_per_method[method].first][methods_with_two_tasks_vertex[method]][currentCost][currentDepthInAbstract] = disjunct2;
							
						   DEBUG(std::cout << "\t\t" << color(GREEN,"2 normal: ") << tasks_per_method[method].first << " " << vertex_to_method[methods_with_two_tasks_vertex[method]] << std::endl);
						   addQ(tasks_per_method[method].first, methods_with_two_tasks_vertex[method], 0, task, to, method, -1, -1, false);
					  }	else {
						   DEBUG(std::cout << "\t\t" << color(YELLOW,"Already known: ") << tasks_per_method[method].first << " " << vertex_to_method[methods_with_two_tasks_vertex[method]] << std::endl);
						   addQ(tasks_per_method[method].first, methods_with_two_tasks_vertex[method], 0, task, to, method, -1, -1, true);
					  }
					}
				}
			}

			step++;
		}
		
		//graph_to_file(htn,"layer" + std::to_string(currentCost) + "_" + std::to_string(currentDepthInAbstract) +  ".dot");

		// handled everything in this layer	
		// switch to next layer	
		std::clock_t layer_end = std::clock();
		double layer_time_in_ms = 1000.0 * (layer_end-layer_start) / CLOCKS_PER_SEC;
		std::cout << "Layer time: " << fixed << layer_time_in_ms << "ms " << fixed << layer_time_in_ms / 1000 << "s " << fixed << layer_time_in_ms / 60000 << "min" << std::endl << std::endl;
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
				DEBUG(std::cout << "Non last layer " << next_abstract_layer->first << std::endl);
				next_abstract_layer++;
				DEBUG(std::cout << "Next last layer " << next_abstract_layer->first << std::endl);
			}
		}
		
		if (next_abstract_layer == abst_q[currentCost].end()){
			// transition to primitive layer
			currentCost = (++prim_q.find(currentCost))->first;
			currentDepthInAbstract = 0; // the primitive layer
			current_queue = prim_q[currentCost];
			std::cout << color(CYAN,"==========================") << " Cost Layer " << currentCost << std::endl; 
		} else {
			currentDepthInAbstract = next_abstract_layer->first;
			current_queue = next_abstract_layer->second;
			std::cout << color(CYAN,"==========================") << " Abstract layer of cost " << currentCost << " layer: " << currentDepthInAbstract << std::endl; 
		}
		std::cout << "Size of Queue for new layer " << current_queue.size() << std::endl;

		//if (currentDepthInAbstract == 5) exit(0);

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
