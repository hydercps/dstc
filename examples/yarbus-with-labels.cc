#include <belief-tracker.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <boost/filesystem.hpp>

typedef std::chrono::high_resolution_clock clock_type;

namespace info = belief_tracker::info::single_info;
typedef belief_tracker::joint::sum::Belief belief_type;
namespace this_reference = belief_tracker::this_reference::single;

/*
namespace info = belief_tracker::info::multi_info;
typedef belief_tracker::joint::sum::Belief belief_type;
namespace this_reference = belief_tracker::this_reference::weak_dontcare;
*/


std::string goal_label_to_str(const std::map<std::string, std::string>& goal_label) {
  std::string str("(");
  unsigned int nb_informable = belief_tracker::Converter::get_informable_slots_size();
  for(unsigned int i = 0 ; i < nb_informable ; ++i) {
    std::string slot_name = belief_tracker::Converter::get_slot(i);
    str += slot_name + "=";
    auto it = goal_label.find(slot_name);
    if(it != goal_label.end())
      str += it->second;
    else
      str += belief_tracker::SYMBOL_UNKNOWN;
    
    if(i != nb_informable -1)
      str += ",";
  }

  str += ")";
  return str;
}

int main(int argc, char * argv[]) {

  if(argc != 5 && argc != 6) {
    std::cerr << "Usage : " << argv[0] << " filename.flist ontology belief_thr verbose(0,1) <session-id>" << std::endl;
    return -1;
  }

  auto clock_begin = clock_type::now();

  std::string flist_filename = argv[1];
  std::string ontology_filename = argv[2];
  double belief_thr = atof(argv[3]);
  bool verbose = atoi(argv[4]);  

  bool process_a_single_dialog = false;
  std::string session_to_process;
  if(argc == 6) {
    process_a_single_dialog = true;
    session_to_process = std::string(argv[5]);
  }

  // We define the set of rules to be used for extracting the informations
  std::list<belief_tracker::info::single_info::rules::rule_type> rules {
    belief_tracker::info::single_info::rules::inform,
      belief_tracker::info::single_info::rules::explconf,
      belief_tracker::info::single_info::rules::implconf,
      belief_tracker::info::single_info::rules::negate,
      belief_tracker::info::single_info::rules::deny
      };



  // We parse the ontology
  belief_tracker::Ontology ontology = belief_tracker::parse_ontology_json_file(ontology_filename);
  // Initialize the static fields of the converter
  belief_tracker::Converter::init(ontology);

  // We parse the flist to access to the dialogs
  if(verbose) std::cout << "Parsing the flist file for getting the dialog and label files" << std::endl;
  auto dialog_label_fullpath = belief_tracker::parse_flist(flist_filename);
  if(verbose) std::cout << "I parsed " << dialog_label_fullpath.size() << " dialogs (and labels if present) " << std::endl;
  if(verbose) std::cout << "done " << std::endl;

  // Compute and set the dataset name
  boost::filesystem::path pathname(flist_filename);
  std::string dataset_name = pathname.filename().string();
  int lastindex = dataset_name.find_last_of("."); 
  dataset_name = dataset_name.substr(0, lastindex); 

  // The stream for saving the tracker output
  std::ostringstream ostr;
  ostr.str("");
  ostr << dataset_name << "-output.json";
  std::ofstream outfile_tracker(ostr.str().c_str());

  // Let us begin the serial save
  belief_tracker::serial::begin_dump(outfile_tracker, dataset_name);
  std::cout << "The tracker output will be saved to " << ostr.str() << std::endl; 

  std::ofstream outfile_size("belief-size.data");
  std::cout << "The size of the belief will be dumped in belief-size.data" << std::endl;


  //////////////////////////////
  // Structures for holding the measures over the dialogs and turns
  std::map<std::string, int> measures {
    {"tot_nb_mistakes", 0},
      {"tot_nb_turns", 0},
	{"nb_dialogs_at_least_one_mistake", 0},
	  {"nb_turns_at_least_one_mistake", 0}
  };

  // A map containing the dialogs on which we make a mistake
  // it maps a session id to the number of errors we make
  std::map<std::string, int> mistaken_dialogs;

  unsigned int dialog_index = 0;
  unsigned int number_of_dialogs  = dialog_label_fullpath.size();
  unsigned int nb_turns;

  // Iterate over the dialogs
  for(auto& dlfpi: dialog_label_fullpath) {

    std::string dialog_fullpath = dlfpi[0];
    std::string label_fullpath = dlfpi[1];
    std::string session_id = dlfpi[2];
    std::string dialog_entry = dlfpi[3];

    if(process_a_single_dialog && session_to_process != session_id)
      continue;

    belief_tracker::Dialog dialog;
    belief_tracker::DialogLabels labels;

    std::list<belief_tracker::DialogTurn>::iterator dialog_turn_iter;
    std::list<belief_tracker::DialogAct> prev_mach_acts;

    dialog = belief_tracker::parse_dialog_json_file(dialog_fullpath);
    labels = belief_tracker::parse_label_json_file(label_fullpath);

    std::cout << '\r' << "Processing dialog " << (dialog_index +1)<< "/" << number_of_dialogs << std::flush;
    if(verbose) std::cout << session_id << std::endl;
    if(verbose)  std::cout << std::endl;


    ////////////////////////////////////
    // Belief initialization
    belief_type belief;
    belief.start();
    if(verbose) std::cout << "Initial belief : " << std::endl << belief << std::endl;
    
    std::map< std::string, double > requested_slots;
    std::map< std::string, double > methods;
    methods["byalternatives"] = 0.0;
    methods["byconstraints"] = 0.0;
    methods["byname"] = 0.0;
    methods["finished"] = 0.0;
    methods["none"] = 1.0;


    /////////////////////
    // Some initializations before looping over the turns
    nb_turns = dialog.turns.size();
    dialog_turn_iter = dialog.turns.begin();

    std::vector<belief_tracker::TrackerSessionTurn> tracker_session;

    std::map<std::string, int> local_measures {
      {"nb_differences", 0},
    	    {"nb_turns_at_least_one_mistake", 0}
    };


    // //////////////////////////////////////////////
    // // We now iterate over the turns of the dialog
    std::map< std::string, std::string > cur_goal_label;
    std::list<double> stats;

    for(unsigned int turn_index = 0; turn_index < nb_turns ; ++turn_index, ++dialog_turn_iter) {
      if(verbose) {
    	std::cout << "****** Turn " << turn_index << " ***** " << dialog.session_id << std::endl;
    	std::cout << std::endl;
    	std::cout << "Dialog path : " << dialog_fullpath << std::endl;
    	std::cout << "Label path : " << label_fullpath << std::endl;
    	std::cout << std::endl;
      }
      auto& slu_hyps = dialog_turn_iter->user_acts;
      auto& macts = dialog_turn_iter->machine_acts;

      if(verbose) std::cout << "Received machine act : " << belief_tracker::dialog_acts_to_str(macts) << std::endl;
      if(verbose) std::cout << "Received hypothesis : " << std::endl << belief_tracker::slu_hyps_to_str(slu_hyps) << std::endl;

      /////// Preprocessing of the machine utterance and SLU hypothesis
      // - Rewriting the repeat() act with the act of the previous turn
      // - Renormalizing the SLU (they do not always sum to 1)
      // - Solving the reference of "this" in the SLU
      belief_tracker::rewrite_repeat_machine_act(macts, prev_mach_acts);
      belief_tracker::renormalize_slu(slu_hyps);
      this_reference::rewrite_slu(slu_hyps, macts);

      // We can now proceed by extracting the informations from the SLU
      auto turn_info = info::extract_info(slu_hyps, macts, rules, verbose);

      // In this example we make use of the labels for computing the mistakes
      cur_goal_label = labels.turns[turn_index].goal_labels;

      // We now use the scored info to update the probability distribution
      // over the values of each slot
      // This produces the tracker output for this dialog
      // We make use of a lazy representation, i.e. we
      // just represent the slot/value pairs for which the belief is != 0
      belief = belief.update(turn_info, belief_tracker::info::transition_function);

      // And the belief is cleaned according to the provided threshold
      // for the DSTC3 with the SJTU SLU, the belief can become really large
      belief.clean(belief_thr);

      // Update the requested slots and methods
      belief_tracker::requested_slots::update(slu_hyps, macts, requested_slots);
      belief_tracker::methods::update(slu_hyps, macts, methods);



      // We can now compute some statistics given the labels
      auto best_goal = belief.extract_best_goal();

      int nb_differences = belief_tracker::nb_differences_goal_to_label(best_goal, cur_goal_label);
      local_measures["nb_differences"] += nb_differences;

      if(nb_differences > 0) 
    	local_measures["nb_turns_at_least_one_mistake"] += 1;

      /****** For debugging  ******/
      if(verbose) {
    	std::cout << "Machine act : " << belief_tracker::dialog_acts_to_str(macts) << std::endl;
    	std::cout << "SLU hyps : " << std::endl;
    	std::cout << belief_tracker::slu_hyps_to_str(slu_hyps) << std::endl;
    	std::cout << "Extracted Turn info : " << std::endl;
    	std::cout << turn_info << std::endl;
    	std::cout << "Belief :" << std::endl;
    	std::cout <<  belief << std::endl;
    	std::cout << "Best goal : " << best_goal.toStr() << std::endl;
    	std::cout << "Labels    : " << goal_label_to_str(cur_goal_label) << std::endl;
    	std::cout << "Methods : " << belief_tracker::methods::toStr(methods) << std::endl;
    	std::cout << "Requested slots : " << belief_tracker::requested_slots::toStr(requested_slots) << std::endl;
    	std::cout << "Nb differences 1best to goal : " << nb_differences << std::endl;
      }
      /******* END DEBUG *****/
      if(verbose) std::cout << std::endl;
      
      outfile_size << belief.belief.size() << std::endl;

      belief_tracker::TrackerSessionTurn tracker_session_turn;
      belief.fill_tracker_session(tracker_session_turn);
      tracker_session_turn.requested_slots_scores = requested_slots;
      tracker_session_turn.method_labels_scores = methods;
      tracker_session.push_back(tracker_session_turn);
      
      // Copy the previous machine acts in order to replace 
      // a repeat if any
      prev_mach_acts = macts;

    } // End of looping over the turns

    // Dump the current session
    belief_tracker::serial::dump_session(outfile_tracker, dialog.session_id, tracker_session, dialog_index == (number_of_dialogs - 1));
    

    // Update the statistics 
    measures["tot_nb_mistakes"] += local_measures["nb_differences"];
    measures["tot_nb_turns"] += nb_turns;
    measures["nb_dialogs_at_least_one_mistake"] += (local_measures["nb_differences"] > 0 ? 1 : 0);
    measures["nb_turns_at_least_one_mistake"] += local_measures["nb_turns_at_least_one_mistake"];

    if(local_measures["nb_differences"] > 0)
      mistaken_dialogs[session_id] = local_measures["nb_differences"];



    if(verbose) std::cout << "fraction of turns the best hyp has 1 diff from the labels : " << double(local_measures["nb_turns_at_least_one_mistake"]) / nb_turns << std::endl;
    
    if(process_a_single_dialog && session_to_process == dialog.session_id) {
      break;
    }
    
    ++dialog_index;
  }
  std::cout << std::endl;




  // Take the clock for computing the walltime
  auto clock_end = clock_type::now();
  double walltime = std::chrono::duration_cast<std::chrono::seconds>(clock_end - clock_begin).count();




  // Display the statistics
  std::cout << measures["nb_dialogs_at_least_one_mistake"] << "/" << dialog_label_fullpath.size() << "(" << double(measures["nb_dialogs_at_least_one_mistake"])/dialog_label_fullpath.size() * 100. << " %)  dialogs have at least one mistake " << std::endl;
  std::cout << "A total number of " << measures["tot_nb_mistakes"] << " mistakes have been done" << std::endl;
  std::cout << "We made at least one mistake in " << measures["nb_turns_at_least_one_mistake"] << " / " << measures["tot_nb_turns"] << " turns, i.e. " << measures["nb_turns_at_least_one_mistake"] * 100. / measures["tot_nb_turns"] << " % (acc 1a = " << 1.0 - (double)measures["nb_turns_at_least_one_mistake"]  / measures["tot_nb_turns"] <<  ") " << std::endl;
  std::cout << "There were a total of " << measures["tot_nb_turns"] << " turns" << std::endl;


  // At the end, we record the dialogs on which a mistake was done
  std::ofstream outfile;
  outfile.open("mistaken_dialogs.data");
  for(auto& kv: mistaken_dialogs)
    outfile << kv.first << '\t' << kv.second << std::endl;
  outfile.close();
  std::cout << "Mistaken dialogs saved in mistaken_dialogs.data" << std::endl;

  // close the file in which the size of the belief is saved
  outfile_size.close();

  // And finally we finish to fill the tracker output
  belief_tracker::serial::end_dump(outfile_tracker, walltime);
  outfile_tracker.close();

}



