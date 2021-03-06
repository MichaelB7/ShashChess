/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>
#include <random>  //variety sugar
#include <fstream> // KellyKinyama mcts
#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

bool pawnsPiecesToEvaluate, passedPawnsToEvaluate,initiativeToEvaluate; //from Shashin Handicap mode
int lessPruningMode;//from Sugar
//kellykynyama mcts begin
bool useExp = true;
bool expHits;
int Movesplayed = 0;
bool startpoint = false;
int openingswritten = 0;
Key OpFileKey[8];
bool pawnEnding = false;
bool SE;
//kellykynyama mcts end
namespace Search {
  LimitsType Limits;
  //from Shashin
  int uciElo;
  bool tal,capablanca,petrosian;
  //end from Shashin
  //mcts begin
  bool perceptronSearch;
  bool persistedSelfLearning;
  //mcts end
}

namespace Tablebases {

  int Cardinality;
  bool RootInTB;
  bool UseRule50;
  Depth ProbeDepth;
}

namespace TB = Tablebases;

using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  // Different node types, used as a template parameter
  enum NodeType { NonPV, PV };

  // Razor and futility margins
  constexpr int RazorMargin = 600;
  Value futility_margin(Depth d, bool improving) {
    return Value((175 - 50 * improving) * d / ONE_PLY);
  }
  int skillLevel;//from Shashin

  // Reductions lookup table, initialized at startup
  int Reductions[64]; // [depth or moveNumber]

  //from Corchess
  // Corchess Reductions lookup tables, initialized at startup
  int ReductionsCC[2][128][64];  // [improving][depth][moveNumber]
  //end from Corchess

  template <bool PvNode> Depth reduction(bool i, Depth d, int mn) {
    int r = Reductions[std::min(d / ONE_PLY, 63)] * Reductions[std::min(mn, 63)] / 1024;
    return ((r + 512) / 1024 + (!i && r > 1024) - PvNode) * ONE_PLY;
  }

  //from Corchess
  template <bool PvNode> Depth reductionCC(bool i, Depth d, int mn) {
    return (ReductionsCC[i][std::min(d / ONE_PLY, 127)][std::min(mn, 63)] - PvNode) * ONE_PLY;
  }
  //end from Corchess

  constexpr int futility_move_count(bool improving, int depth) {
    return (5 + depth * depth) * (1 + improving) / 2;
  }

  // History and stats update bonus, based on depth
  int stat_bonus(Depth depth) {
    int d = depth / ONE_PLY;
    return d > 17 ? 0 : 29 * d * d + 138 * d - 134;
  }

  // Add a small random component to draw evaluations to avoid 3fold-blindness
  Value value_draw(Depth depth, Thread* thisThread) {
    return depth < 4 ? VALUE_DRAW
                     : VALUE_DRAW + Value(2 * (thisThread->nodes & 1) - 1);
  }

  // Skill structure is used to implement strength limit
  struct Skill {
    explicit Skill(int l) : level(l) {}
    bool enabled() const { return level < 20; }
    bool time_to_pick(Depth depth) const { return depth / ONE_PLY == 1 + level; }
    Move pick_best(size_t multiPV);

    int level;
    Move best = MOVE_NONE;
  };
  bool limitStrength ;//from Shashin
  int variety;//from Sugar
  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);

  template <NodeType NT>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = DEPTH_ZERO);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply);
  void update_pv(Move* pv, Move move, Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_quiet_stats(const Position& pos, Stack* ss, Move move, Move* quiets, int quietCount, int bonus);
  void update_capture_stats(const Position& pos, Move move, Move* captures, int captureCount, int bonus);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2 * ONE_PLY);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= ONE_PLY)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - ONE_PLY);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

  //perceptron_scratch begin
  constexpr int percInput     = 4;
  constexpr int percOutput    = 3;
  float perceptronWeights[percInput + 1][percOutput];

  int infer(float input[percInput]){
      float continuousClasses[percOutput];
      float bestFit     = -100000000.0;
      int   bestClass   = -1;
      std::memset(continuousClasses, 0.0, sizeof continuousClasses);

      for (int d1 = 0; d1 < percOutput; d1++){
          continuousClasses[d1] += perceptronWeights[0][d1]; // bias
          for (int d2 = 0; d2 < percInput; d2++){
              continuousClasses[d1] += perceptronWeights[1 + d2][d1] * input[d2];
          }
          if (bestFit < continuousClasses[d1]){
             bestFit = continuousClasses[d1];
             bestClass = d1;
          }
      }
      return bestClass;
  }

  void train(float input[percInput], float rate){
      for (int d1 = 0; d1 < percOutput; d1++){
          perceptronWeights[0][d1] -= ((perceptronWeights[0][d1]  > 0) - (perceptronWeights[0][d1]  < 0)) * rate;
          for (int d2 = 0; d2 < percInput; d2++){
              perceptronWeights[1 + d2][d1] -=  ((perceptronWeights[1 + d2][d1] > 0) - (perceptronWeights[1 + d2][d1] < 0)) * input[d2] * rate;
          }
      }
  }

  //perceptron_scratch end
} // namespace


/// Search::init() is called at startup to initialize various lookup tables

void Search::init() {

  for (int i = 1; i < 64; ++i)
      Reductions[i] = int(1024 * std::log(i) / std::sqrt(1.95));
  //perceptron_scratch begin
  for (int d1 = 0; d1 <= percInput; d1++)
    for (int d2 = 0; d2 < percOutput; d2++)
    {
      perceptronWeights[d1][d2] = float(d1*d2) - percInput*percOutput / 4.0;
    }
  //perceptron_scratch end
  //from Corchess
  for (int imp = 0; imp <= 1; ++imp)
      for (int d = 1; d < 128; ++d)
          for (int mc = 1; mc < 64; ++mc)
          {
              double r = 0.215 * d * (1.0 - exp(-8.0 / d)) * log(mc);

              ReductionsCC[imp][d][mc] = std::round(r);

              // Increase reduction for non-PV nodes when eval is not improving
              if (!imp && r > 1.0)
                ReductionsCC[imp][d][mc]++;
          }
  //end from Corchess
}


/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();

  Time.availableNodes = 0;
  TT.clear();
  Threads.clear();
  Tablebases::init(Options["SyzygyPath"]); // Free mapped files
}


/// MainThread::search() is started when the program receives the UCI 'go'
/// command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {

  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft * ONE_PLY);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }
  //from Sugar
  limitStrength	    = Options["UCI_LimitStrength"];
  //end from Sugar
  Color us = rootPos.side_to_move();
  Time.init(Limits, us, rootPos.game_ply());
  TT.new_search();
  //mcts begin
  perceptronSearch=Options["NN Perceptron Search"];
  persistedSelfLearning=Options["NN Persisted Self-Learning"];
  //mcts end
  //KellyKinyama mcts begin
  int piecesCnt=0;
  if(persistedSelfLearning){
	  expHits = false;
	  piecesCnt = rootPos.count<KNIGHT>(WHITE) + rootPos.count<BISHOP>(WHITE) + rootPos.count<ROOK>(WHITE) + rootPos.count<QUEEN>(WHITE) + rootPos.count<KING>(WHITE)
		  + rootPos.count<KNIGHT>(BLACK) + rootPos.count<BISHOP>(BLACK) + rootPos.count<ROOK>(BLACK) + rootPos.count<QUEEN>(BLACK) + rootPos.count<KING>(BLACK);
	
	  if (piecesCnt <= 8 && !pawnEnding)
	  {
		  pawnEnding = true;
		  EXPawnresize();
	  }
	  if (piecesCnt <= 8)
	  {
		  useExp = true;
	  }
  }	
  //KellyKinyama mcts end
  //from Sugar
  lessPruningMode = Options["Less Pruning Mode"];
  variety = Options["Variety"];
  //end from Sugar
  //from Shashin
  uciElo=Options["UCI_Elo"];
  tal=Options["Tal"];
  capablanca=Options["Capablanca"];
  petrosian=Options["Petrosian"];
  pawnsPiecesToEvaluate = uciElo >= 2000;
  passedPawnsToEvaluate = uciElo>=2200;
  initiativeToEvaluate= uciElo>=2400;
  skillLevel= ((int)((uciElo-1500)/65));
  //end from Shashin

  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW)
                << sync_endl;
  }
  else
  {
      for (Thread* th : Threads)
          if (th != this)
              th->start_searching();

      Thread::search(); // Let's start searching!
  }

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands.

  while (!Threads.stop && (ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  for (Thread* th : Threads)
      if (th != this)
          th->wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.availableNodes += Limits.inc[us] - Threads.nodes_searched();

  Thread* bestThread = this;

  // Check if there are threads with a better score than main thread
  if (    Options["MultiPV"] == 1
      && !Limits.depth
      && !limitStrength //From Shashin
      &&  rootMoves[0].pv[0] != MOVE_NONE)
  {
      std::map<Move, int64_t> votes;
      Value minScore = this->rootMoves[0].score;

      // Find out minimum score and reset votes for moves which can be voted
      for (Thread* th: Threads)
          minScore = std::min(minScore, th->rootMoves[0].score);

      // Vote according to score and depth
      for (Thread* th : Threads)
      {
          int64_t s = th->rootMoves[0].score - minScore + 1;
          votes[th->rootMoves[0].pv[0]] += 200 + s * s * int(th->completedDepth);
      }

      // Select best thread
      auto bestVote = votes[this->rootMoves[0].pv[0]];
      for (Thread* th : Threads)
          if (votes[th->rootMoves[0].pv[0]] > bestVote)
          {
              bestVote = votes[th->rootMoves[0].pv[0]];
              bestThread = th;
          }
  }

  previousScore = bestThread->rootMoves[0].score;
  //kellykynyama mcts begin
  if(persistedSelfLearning){
	  if ((((Movesplayed <= 40) || (piecesCnt <= 6)) && (bestThread->completedDepth > 4 * ONE_PLY)))
	  {
		  std::ofstream general("experience.bin", std::ofstream::app | std::ofstream::binary);
		  ExpEntry tempExpEntry;
		  tempExpEntry.depth = bestThread->completedDepth;
		  tempExpEntry.hashkey = rootPos.key();
		  tempExpEntry.move = bestThread->rootMoves[0].pv[0];
		  tempExpEntry.score = bestThread->rootMoves[0].score;
		  if (Movesplayed <= 10 && startpoint &&  piecesCnt > 6)
		  {
			  general.write((char*)&tempExpEntry, sizeof(tempExpEntry));
		  }
		  if (startpoint &&  piecesCnt > 6)
		  {
			  for (int x = 0; x < openingswritten; x++)
			  {
				  string openings;
				  char *opnings;
	
				  std::ostringstream ss;
				  ss << OpFileKey[x];
				  openings = ss.str() + ".bin";
				  opnings = new char[openings.length() + 1];
				  std::strcpy(opnings, openings.c_str());
				  std::ofstream myFile(opnings, std::ofstream::app | std::ofstream::binary);
				  myFile.write((char*)&tempExpEntry, sizeof(tempExpEntry));
				  myFile.close();
			  }
		  }
		  if (piecesCnt <= 2)
		  {
			  std::ofstream pawngame("pawngame.bin", std::ofstream::app | std::ofstream::binary);
			  pawngame.write((char*)&tempExpEntry, sizeof(tempExpEntry));
			  pawngame.close();
		  }
		  Movesplayed++;
	
	  }
	
	  if (!expHits)
	  {
		  useExp = false;
	  }
  }
  //kellykynyama mcts end
  // Send again PV info if we have a new best thread
  if (bestThread != this)
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;

  sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

  std::cout << sync_endl;
}

//from Shashin
inline uint8_t getShashinValue(Value score) {
	if ((int)score < -SHASHIN_TAL_THRESHOLD) {
		return SHASHIN_POSITION_PETROSIAN;
	}
	if (((int)score >= -SHASHIN_TAL_THRESHOLD)
			&& ((int)score <= -SHASHIN_CAPABLANCA_THRESHOLD)) {
		return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
	}
	if (((int)score < SHASHIN_CAPABLANCA_THRESHOLD)) {
		return SHASHIN_POSITION_CAPABLANCA;
	}
	if (((int)score >= SHASHIN_CAPABLANCA_THRESHOLD)
			&& ((int)score <= SHASHIN_TAL_THRESHOLD)) {
		return SHASHIN_POSITION_TAL_CAPABLANCA;
	}
	if ((int)score > SHASHIN_TAL_THRESHOLD) {
		return SHASHIN_POSITION_TAL;
	}
	return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
}

inline int getShashinQuiescentCapablanca(Value score,int refScore) {
  return abs(score) > refScore ? 0 : 1;
}

inline int getShashinMaxLmr(Value score){
  if(abs(score) <= SHASHIN_MIDDLE_HIGH_SCORE){
      return (SHASHIN_MAX_LMR * ONE_PLY);
  }
  if(abs(score) <= SHASHIN_MAX_SCORE){
      return (-abs(score) + MLR2)/MLR3;
  }
  return (SHASHIN_MIN_LMR * ONE_PLY);
}

inline uint8_t getInitialShashinValue() {
	if (!tal && !capablanca
			&& !petrosian)
		return SHASHIN_POSITION_DEFAULT;

	if (tal && capablanca
			&& !petrosian)
		return SHASHIN_POSITION_TAL_CAPABLANCA;

	if (tal && !capablanca
			&& !petrosian)
		return SHASHIN_POSITION_TAL;

	if (!tal && capablanca
			&& !petrosian)
		return SHASHIN_POSITION_CAPABLANCA;

	if (!tal && capablanca
			&& petrosian)
		return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;

	if (!tal && !capablanca
			&& petrosian)
		return SHASHIN_POSITION_PETROSIAN;

	if (tal && capablanca
			&& petrosian)
		return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;

	return SHASHIN_POSITION_TAL_PETROSIAN;
}

inline int getInitialContemptByShashin() {
	if (!tal && !capablanca
			&& !petrosian)
		return SHASHIN_DEFAULT_CONTEMPT;

	if (tal && capablanca
			&& !petrosian)
		return SHASHIN_TAL_CAPABLANCA_CONTEMPT;

	if (tal && !capablanca
			&& !petrosian)
		return SHASHIN_TAL_CONTEMPT;

	if (!tal && capablanca
			&& !petrosian)
		return SHASHIN_CAPABLANCA_CONTEMPT;

	if (!tal && capablanca
			&& petrosian)
		return SHASHIN_CAPABLANCA_PETROSIAN_CONTEMPT;

	if (!tal && !capablanca
			&& petrosian)
		return SHASHIN_PETROSIAN_CONTEMPT;

	if (tal && capablanca
			&& petrosian)
		return SHASHIN_TAL_CAPABLANCA_PETROSIAN_CONTEMPT;

	return SHASHIN_TAL_PETROSIAN_CONTEMPT;
}

inline int getInitialShashinMaxLmr(){
  if((!capablanca && tal)
	||
	(!capablanca && petrosian)){
	return (SHASHIN_MIN_LMR * ONE_PLY);
  }
  if((tal && capablanca && petrosian)
	||
	(!tal && !petrosian)){
	return (SHASHIN_MAX_LMR * ONE_PLY);
  }
  return (SHASHIN_MIDDLE_LMR * ONE_PLY);


    if((!petrosian && !tal)
	||
	(!petrosian && capablanca)
	||
	(!tal && capablanca)){
	return (SHASHIN_MAX_LMR * ONE_PLY);
    }
    return (SHASHIN_MIN_LMR * ONE_PLY);
}

inline int getInitialShashinQuiescent(){
    if ((!tal && !capablanca
		    && !petrosian)
	||
	(!tal && capablanca
			    && !petrosian))
	    return 1;
      return 0;
}

void Thread::initShashinElements ()
{
  shashinValue = getInitialShashinValue ();
  shashinContempt = getInitialContemptByShashin ();
  shashinQuiescentCapablancaMaxScore = getInitialShashinQuiescent ();
  shashinMaxLmr=getInitialShashinMaxLmr();
}

void Thread::updateShashinValues (Value score, int ct, Color us, Value value)
{
  // Adjust contempt based on value (dynamic contempt)
  int dct = ct + 88 * value / (abs (value) + 200);
  contempt = (
      us == WHITE ? make_score (dct, dct / 2) : -make_score (dct, dct / 2));
  Value scoreCP = (Value)(score * scoreScale / PawnValueEg);
  shashinValue = getShashinValue (scoreCP);
  shashinQuiescentCapablancaMaxScore =
      getShashinQuiescentCapablanca (scoreCP, SHASHIN_MAX_SCORE);
  shashinMaxLmr=getShashinMaxLmr(scoreCP);
}

//end from Shashin
/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  // To allow access to (ss-7) up to (ss+2), the stack must be oversized.
  // The former is needed to allow update_continuation_histories(ss-1, ...),
  // which accesses its argument at ss-6, also near the root.
  // The latter is needed for statScores and killer initialization.
  Stack stack[MAX_PLY+10], *ss = stack+7;
  Move  pv[MAX_PLY+1];
  Value bestValue, alpha, beta, delta, delta1, delta2; //from Corchess
  Move  lastBestMove = MOVE_NONE;
  Depth lastBestMoveDepth = DEPTH_ZERO;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1.0;
  Color us = rootPos.side_to_move();

  std::memset(ss-7, 0, 10 * sizeof(Stack));
  for (int i = 7; i > 0; i--)
     (ss-i)->continuationHistory = &this->continuationHistory[NO_PIECE][0]; // Use as sentinel
  ss->pv = pv;
  if(lessPruningMode)
  {
      bestValue = delta1 = delta2 = alpha = -VALUE_INFINITE;
  }
  else
  {
      bestValue = delta = alpha = -VALUE_INFINITE;
  }
  beta = VALUE_INFINITE;

  if (mainThread)
      mainThread->bestMoveChanges = 0;

  size_t multiPV = Options["MultiPV"];
  Skill skill(skillLevel);//from Shashin
  if (lessPruningMode) multiPV = size_t(pow(2, lessPruningMode-1));//from Sugar adapted to corchess
  // When playing with strength handicap enable MultiPV search that we will
  // use behind the scenes to retrieve a set of possible moves.
  if (skill.enabled() && limitStrength)//from Shashin
      multiPV = std::max(multiPV, (size_t)4);

  multiPV = std::min(multiPV, rootMoves.size());
  //from Shashin
  initShashinElements ();
 //end from Shashin

  int ct = (shashinContempt) * PawnValueEg / 100; // From centipawns Shashin
  // In analysis mode, adjust contempt in accordance with user preference
  if (Limits.infinite || Options["UCI_AnalyseMode"])
      ct =  Options["Analysis Contempt"] == "Off"  ? 0
          : Options["Analysis Contempt"] == "Both" ? ct
          : Options["Analysis Contempt"] == "White" && us == BLACK ? -ct
          : Options["Analysis Contempt"] == "Black" && us == WHITE ? -ct
          : ct;

  // Evaluation score is from the white point of view
  contempt = (us == WHITE ?  make_score(ct, ct / 2)
                          : -make_score(ct, ct / 2));

  // Iterative deepening loop until requested to stop or the target depth is reached
  while (   (rootDepth += ONE_PLY) < DEPTH_MAX
         && !Threads.stop
         && !(Limits.depth && mainThread && rootDepth / ONE_PLY > Limits.depth))
  {
      // Age out PV variability metric
      if (mainThread)
          mainThread->bestMoveChanges *= 0.517;

      // Save the last iteration's scores before first PV line is searched and
      // all the move scores except the (new) PV are set to -VALUE_INFINITE.
      for (RootMove& rm : rootMoves)
          rm.previousScore = rm.score;

      size_t pvFirst = 0;
      pvLast = 0;

      //mcts Cardanobile from joergoster begin
      if(perceptronSearch)
	{
	  // Reset mcts values
	  visits = 0;
	  allScores = 0;
	}
      //mcts Cardanobile from joergoster end
      // MultiPV loop. We perform a full root search for each PV line
      for (pvIdx = 0; pvIdx < multiPV && !Threads.stop; ++pvIdx)
      {
          if (pvIdx == pvLast)
          {
              pvFirst = pvLast;
              for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                  if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                      break;
          }

          // Reset UCI info selDepth for each depth and each PV line
          selDepth = 0;

          // Reset aspiration window starting size
          if (rootDepth >= 5 * ONE_PLY)
          {
              Value previousScore = rootMoves[pvIdx].previousScore;
              //from Corchess
              if(lessPruningMode)
              {
                  delta1 = (previousScore < 0) ? Value(int(12.0 + 0.07 * abs(previousScore))) : Value(16);
                  delta2 = (previousScore > 0) ? Value(int(12.0 + 0.07 * abs(previousScore))) : Value(16);
                  alpha = std::max(previousScore - delta1,-VALUE_INFINITE);
                  beta  = std::min(previousScore + delta2, VALUE_INFINITE);
              }
              else
              {
        	  delta = Value(20);
                  alpha = std::max(previousScore - delta,-VALUE_INFINITE);
                  beta  = std::min(previousScore + delta, VALUE_INFINITE);
              }
              //end from Corchess
              updateShashinValues(previousScore, ct, us, previousScore); //from Shashin
          }

          // Start with a small aspiration window and, in the case of a fail
          // high/low, re-search with a bigger window until we don't fail
          // high/low anymore.
          int failedHighCnt = 0;
          while (true)
          {
              Depth adjustedDepth = std::max(ONE_PLY, rootDepth - failedHighCnt * ONE_PLY);
              bestValue = ::search<PV>(rootPos, ss, alpha, beta, adjustedDepth, false);
              updateShashinValues (bestValue,ct, us, bestValue); //from Shashin

              // Bring the best move to the front. It is critical that sorting
              // is done with a stable algorithm because all the values but the
              // first and eventually the new best one are set to -VALUE_INFINITE
              // and we want to keep the same order for all the moves except the
              // new PV that goes to the front. Note that in case of MultiPV
              // search the already searched PV lines are preserved.
              std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

              // If search has been stopped, we break immediately. Sorting is
              // safe because RootMoves is still valid, although it refers to
              // the previous iteration.
              if (Threads.stop)
                  break;

              // When failing high/low give some update (without cluttering
              // the UI) before a re-search.
              if (   mainThread
                  && multiPV == 1
                  && (bestValue <= alpha || bestValue >= beta)
                  && Time.elapsed() > 3000)
                  sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

              // In case of failing low/high increase aspiration window and
              // re-search, otherwise exit the loop.
              if (bestValue <= alpha ||
        	  //mcts Cardanobile from joergoster begin
        	  (((rootPos.this_thread()->shashinValue!=SHASHIN_POSITION_TAL)
        	  && (rootPos.this_thread()->shashinValue!=SHASHIN_POSITION_PETROSIAN) && perceptronSearch)
        	  && (Value(rootMoves[0].zScore / rootMoves[0].visits) <= alpha - PawnValueMg / 2)))
        	 //mcts Cardanobile from joergoster end
              {
                  beta = (alpha + beta) / 2;
                  if(lessPruningMode)
                  {
                      alpha = std::max(bestValue - delta1, -VALUE_INFINITE);
                  }
                  else
                  {
                      alpha = std::max(bestValue - delta, -VALUE_INFINITE);
                  }

                  if (mainThread)
                  {
                      failedHighCnt = 0;
                      mainThread->stopOnPonderhit = false;
                  }
              }
              else if (bestValue >= beta)
              {
        	  if(lessPruningMode)
        	  {
        	      beta = std::min(bestValue + delta2, VALUE_INFINITE);
        	  }
        	  else
        	  {
        	      beta = std::min(bestValue + delta, VALUE_INFINITE);
        	  }
                  if (mainThread)
                      ++failedHighCnt;
              }
              else
                  break;

              if(lessPruningMode)
              {
                  delta1 += delta1 / 4 + 5;
                  delta2 += delta2 / 4 + 5;
              }
              else
              {
                  delta += delta / 4 + 5;
              }

              assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
          }

          // Sort the PV lines searched so far and update the GUI
          std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

          if (    mainThread
              && (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
              sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
      }

      if (!Threads.stop)
          completedDepth = rootDepth;

      if (rootMoves[0].pv[0] != lastBestMove) {
         lastBestMove = rootMoves[0].pv[0];
         lastBestMoveDepth = rootDepth;
      }

      // Have we found a "mate in x"?
      if (   Limits.mate
          && bestValue >= VALUE_MATE_IN_MAX_PLY
          && VALUE_MATE - bestValue <= 2 * Limits.mate)
          Threads.stop = true;

      if (!mainThread)
          continue;

      // If skill level is enabled and time is up, pick a sub-optimal best move
      if (skill.enabled() && skill.time_to_pick(rootDepth) && limitStrength) //from Shashin
          skill.pick_best(multiPV);

      // Do we have time for the next iteration? Can we stop searching now?
      if (    Limits.use_time_management()
          && !Threads.stop
          && !mainThread->stopOnPonderhit)
      {
          double fallingEval = (306 + 9 * (mainThread->previousScore - bestValue)) / 581.0;
          fallingEval = clamp(fallingEval, 0.5, 1.5);

          // If the bestMove is stable over several iterations, reduce time accordingly
          timeReduction = lastBestMoveDepth + 10 * ONE_PLY < completedDepth ? 1.95 : 1.0;
          double reduction = std::pow(mainThread->previousTimeReduction, 0.528) / timeReduction;

          // Use part of the gained time from a previous stable move for the current move
          double bestMoveInstability = 1.0 + mainThread->bestMoveChanges;

          // Stop the search if we have only one legal move, or if available time elapsed
          if (   rootMoves.size() == 1
              || Time.elapsed() > Time.optimum() * fallingEval * reduction * bestMoveInstability)
          {
              // If we are allowed to ponder do not stop the search now but
              // keep pondering until the GUI sends "ponderhit" or "stop".
              if (mainThread->ponder)
                  mainThread->stopOnPonderhit = true;
              else
                  Threads.stop = true;
          }
      }
      //mcts cardanobile playout begin
      if ((mainThread && !Threads.stop)
	  && (rootPos.this_thread()->shashinValue!=SHASHIN_POSITION_TAL) && (rootPos.this_thread()->shashinValue!=SHASHIN_POSITION_PETROSIAN) && perceptronSearch)
      {
	  playout(lastBestMove, ss, bestValue);
      }
      //mcts cardanobile playout end
  }

  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;

  // If skill level is enabled, swap best PV line with the sub-optimal one
  if (skill.enabled() && limitStrength)//from Shashin
      std::swap(rootMoves[0], *std::find(rootMoves.begin(), rootMoves.end(),
                skill.best ? skill.best : skill.pick_best(multiPV)));
}

// Playout a game, in the hope of meaningfully filling the TT beyond the horizon
Value Thread::playout(Move playMove, Stack* ss, Value playoutValue) {
    StateInfo st;
    bool ttHit;

    if (     Threads.stop
        ||  !rootPos.pseudo_legal(playMove)
        ||  !rootPos.legal(playMove))
        return VALUE_NONE;

    if (rootPos.is_draw(ss->ply))
        return VALUE_DRAW;

    ss->currentMove         = playMove;
    ss->continuationHistory = &continuationHistory[rootPos.moved_piece(playMove)][to_sq(playMove)];

    rootPos.do_move(playMove, st);

    (ss+1)->ply = ss->ply + 1;
    int d = int(rootDepth) * int(rootDepth) / (rootDepth + 4 * ONE_PLY) - 2;
	Depth newDepth  = d * ONE_PLY;
    TTEntry* tte    = TT.probe(rootPos.key(), ttHit);
	if (!ttHit && MoveList<LEGAL>(rootPos).size()){
	    playoutValue = ::search<NonPV>(rootPos, ss+1, - playoutValue,  - playoutValue + 1, newDepth, true);
	    tte    = TT.probe(rootPos.key(), ttHit);
	   }

    Move ttMove  = ttHit ? tte->move() : MOVE_NONE;
    if(  ttHit
      && ttMove != MOVE_NONE
      && ss->ply < MAX_PLY - 2
      && abs(playoutValue) < VALUE_KNOWN_WIN)
        playoutValue = - playout(ttMove, ss+1, - playoutValue);

    rootPos.undo_move(playMove);
	return playoutValue;
}
namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType NT>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode = NT == PV;
    const bool rootNode = PvNode && ss->ply == 0;

    Thread* thisThread = pos.this_thread(); //mcts Cardanobile from joergoster

    // Check if we have an upcoming move which draws by repetition, or
    // if the opponent had an alternative move earlier to this position.
    if (   pos.rule50_count() >= 3
        && alpha < VALUE_DRAW
        && !rootNode
        && pos.has_game_cycle(ss->ply))
    {
        alpha = value_draw(depth, pos.this_thread());
        if (alpha >= beta)
          //mcts Cardanobile from joergoster begin
          {
            if(perceptronSearch)
              {
                thisThread->visits++;
                thisThread->allScores += (ss->ply % 2 == 0) ? alpha : -alpha;
              }
            return alpha;
          }
          //mcts Cardanobile from joergoster end
    }

    // Dive into quiescence search when the depth reaches zero
    if (depth < ONE_PLY)
      //mcts Cardanobile from joergoster begin
      {
        Value qs = qsearch<NT>(pos, ss, alpha, beta);
        if(perceptronSearch)
          {
            thisThread->visits++;
            thisThread->allScores += (ss->ply % 2 == 0) ? qs : -qs;
          }
        return qs;
      }
      //mcts Cardanobile from joergoster end

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(DEPTH_ZERO < depth && depth < DEPTH_MAX);
    assert(!(PvNode && cutNode));
    assert(depth / ONE_PLY * ONE_PLY == depth);

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
	//from MateFinder
    TTEntry* tte=NULL;
    Key posKey=0;
	//end from MateFinder
    Move ttMove, move, excludedMove=MOVE_NONE, bestMove,expttMove=MOVE_NONE;//from MateFinder and kellykynyama mcts
    Depth extension, newDepth;
    Value bestValue, value, ttValue, eval, maxValue, pureStaticEval, expttValue=VALUE_NONE;//kellykynyama mcts
    bool ttHit, ttPv, inCheck, givesCheck, improving, expttHit=false;//kellykynyama mcts
    bool captureOrPromotion, doFullDepthSearch, moveCountPruning, ttCapture;
    Piece movedPiece;

    //from perceptron_scratch  begin
    int moveCount, captureCount, quietCount, prediction;
    float features[percInput] = {0.0, 0.0, 0.0, 0.0};
    bool trainPerc = false;
    //from perceptron_scratch end

    // Step 1. Initialize node
    // Thread* thisThread = pos.this_thread(); //mcts Cardanobile from joergoster
    inCheck = pos.checkers();
    Color us = pos.side_to_move();
    moveCount = captureCount = quietCount = ss->moveCount = 0;
    bestValue = -VALUE_INFINITE;
    maxValue = VALUE_INFINITE;

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    if (!rootNode)
    {
        // Step 2. Check for aborted search and immediate draw
        if (   Threads.stop.load(std::memory_order_relaxed)
            || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
          //mcts Cardanobile from joergoster begin
          {
            Value draw = value_draw(depth, pos.this_thread());
            if(perceptronSearch)
              {
                thisThread->visits++;
                thisThread->allScores += (ss->ply % 2 == 0) ? draw : -draw;
              }
            return (ss->ply >= MAX_PLY && !inCheck) ? evaluate(pos) : draw;
            //mcts Cardanobile from joergoster end
          }

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply+1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ply = ss->ply + 1;
    ss->currentMove = (ss+1)->excludedMove = bestMove = MOVE_NONE;
    ss->continuationHistory = &thisThread->continuationHistory[NO_PIECE][0];
    (ss+2)->killers[0] = (ss+2)->killers[1] = MOVE_NONE;
    Square prevSq = to_sq((ss-1)->currentMove);

    // Initialize statScore to zero for the grandchildren of the current position.
    // So statScore is shared between all grandchildren and only the first grandchild
    // starts with statScore = 0. Later grandchildren start with the last calculated
    // statScore of the previous grandchild. This influences the reduction rules in
    // LMR which are based on the statScore of parent position.
    (ss+2)->statScore = 0;

    // Step 4. Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = pos.key() ^ Key(excludedMove << 16); // Isn't a very good hash
    tte = TT.probe(posKey, ttHit);
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ttHit    ? tte->move() : MOVE_NONE;
    ttPv = (ttHit && tte->is_pv()) || (PvNode && depth > 4 * ONE_PLY);

    // if position has been searched at higher depths and we are shuffling, return value_draw
    if (pos.rule50_count() > 36
        && ss->ply > 36
        && depth < 3 * ONE_PLY
        && ttHit
        && tte->depth() > depth
        && pos.count<PAWN>() > 0)
        return VALUE_DRAW;

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ttHit
        && tte->depth() >= depth
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                if (!pos.capture_or_promotion(ttMove))
                    update_quiet_stats(pos, ss, ttMove, nullptr, 0, stat_bonus(depth));

                // Extra penalty for a quiet TT or main killer move in previous ply when it gets refuted
                if (    ((ss-1)->moveCount == 1 || (ss-1)->currentMove == (ss-1)->killers[0])
                     && !pos.captured_piece())
                        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));
            }
            // Penalty for a quiet ttMove that fails low
            else if (!pos.capture_or_promotion(ttMove))
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][from_to(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }
        //mcts Cardanobile from joergoster begin
        if(perceptronSearch)
          {
            thisThread->visits++;
            thisThread->allScores += (ss->ply % 2 == 0) ? ttValue : -ttValue;
          }
        //mcts Cardanobile from joergoster end
        return ttValue;
    }

	//mcts kynyama begin
	bool updated = false;
	int visits = 0;
	int minSons = 0;
	if(persistedSelfLearning){
		expttHit = false;
		minSons = 0;
		visits = 0;
		updated = false;
	
		if (!excludedMove && useExp)
		{
			Node node = get_node(posKey);
			if (node!=nullptr)
			{
				Child child = node->child[0];
				if (node->hashkey == posKey)
				{
					bool ttMovehave = false;
					if (ttMove)
						ttMovehave = true;
					expHits = true;
					expttHit = true;
					Value myValue = -VALUE_INFINITE;
					minSons = node->sons;
					visits = node->totalVisits;
	
					if (node->child[node->sons - 1].depth >= depth)
					{
						myValue = node->child[node->sons - 1].score;
						expttMove = node->child[node->sons - 1].move;
						expttHit = true;
						expttValue = node->child[node->sons - 1].score;
						updated = true;
						child = node->child[node->sons - 1];
	
						if (!ttMovehave)
						{
							ttMove = node->child[node->sons - 1].move;
						}
					}
	
	
	
					if (!ttHit && updated
						&& child.depth >= depth
						)
					{
						tte->save(posKey, child.score, ttPv, BOUND_EXACT, child.depth, child.move, child.score);
	
						tte = TT.probe(posKey, ttHit);
						ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;
						ttMove = rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
							: ttHit ? tte->move() : MOVE_NONE;
					}
					if (!PvNode && updated
						&& child.depth >= depth
						)
					{
						if (child.score >= beta)
						{
							if (!pos.capture_or_promotion(child.move))
								update_quiet_stats(pos, ss, child.move, nullptr, 0, stat_bonus(depth));
	
							// Extra penalty for a quiet TT move in previous ply when it gets refuted
							if ((ss - 1)->moveCount == 1 && !pos.captured_piece())
								update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));
						}
						thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);
						return myValue;
					}
					  //from old searchMCTS
					  if (!rootNode && updated
						  && child.depth >= depth
						  )
					  {
						  if (child.score >= beta)
						  {
							  if (!pos.capture_or_promotion(child.move))
								  update_quiet_stats(pos, ss, child.move, nullptr, 0, stat_bonus(depth));
	
							  // Extra penalty for a quiet TT move in previous ply when it gets refuted
							  if ((ss - 1)->moveCount == 1 && !pos.captured_piece())
								  update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));
						  }
						  thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);
						  return myValue;
					  }
					  //end from old searchMCTS
				}
	
			}
		}	
	}
	//mcts kynyama end

    // Step 5. Tablebases probe
    if (!rootNode && TB::Cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES>();

        if (    piecesCount <= TB::Cardinality
            && (piecesCount <  TB::Cardinality || depth >= TB::ProbeDepth)
            &&  pos.rule50_count() == 0
            && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore wdl = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (thisThread == Threads.main())
                static_cast<MainThread*>(thisThread)->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                int drawScore = TB::UseRule50 ? 1 : 0;

                value =  wdl < -drawScore ? -VALUE_MATE + MAX_PLY + ss->ply + 1
                       : wdl >  drawScore ?  VALUE_MATE - MAX_PLY - ss->ply - 1
                                          :  VALUE_DRAW + 2 * wdl * drawScore;

                Bound b =  wdl < -drawScore ? BOUND_UPPER
                         : wdl >  drawScore ? BOUND_LOWER : BOUND_EXACT;

                if (    b == BOUND_EXACT
                    || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                {
                    tte->save(posKey, value_to_tt(value, ss->ply), ttPv, b,
                              std::min(DEPTH_MAX - ONE_PLY, depth + 6 * ONE_PLY),
                              MOVE_NONE, VALUE_NONE);
                    //mcts Cardanobile from joergoster begin
                    if(perceptronSearch)
                      {
                        thisThread->visits++;
                        thisThread->allScores += (ss->ply % 2 == 0) ? value : -value;
                      }
                    //mcts Cardanobile from joergoster end
                    return value;
                }

                if (PvNode)
                {
                    if (b == BOUND_LOWER)
                        bestValue = value, alpha = std::max(alpha, bestValue);
                    else
                        maxValue = value;
                }
            }
        }
    }

    // Step 6. Static evaluation of the position
    if (inCheck)
    {
        ss->staticEval = eval = pureStaticEval = VALUE_NONE;
        improving = false;
        goto moves_loop;  // Skip early pruning when in check
    }
    else if (ttHit)
    {
        // Never assume anything on values stored in TT
        ss->staticEval = eval = pureStaticEval = tte->eval();
        if (eval == VALUE_NONE)
            ss->staticEval = eval = pureStaticEval = evaluate(pos);

        // Can ttValue be used as a better position evaluation?
        if (    ttValue != VALUE_NONE
            && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {	//kellykinyama mcts begin
		if (!ttHit && expttHit && updated && persistedSelfLearning)
		{
			// Never assume anything on values stored in TT
			ss->staticEval = eval = pureStaticEval = expttValue;
			if (eval == VALUE_NONE)
				ss->staticEval = eval = pureStaticEval = evaluate(pos);


		}
		else
		{
			if ((ss-1)->currentMove != MOVE_NULL)
			{
				int bonus = -(ss-1)->statScore / 512;

            	pureStaticEval = evaluate(pos);
            	ss->staticEval = eval = pureStaticEval + bonus;
			}
			else{
			    ss->staticEval = eval = pureStaticEval = -(ss-1)->staticEval + 2 * Eval::Tempo;
			}
			tte->save(posKey, VALUE_NONE, ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, pureStaticEval);
		}
	}
	//kellykyniama mcts end

    // Step 7. Razoring (~2 Elo)
    if (   !rootNode // The required rootNode PV handling is not available in qsearch
        &&  depth < 2 * ONE_PLY
        &&  eval <= alpha - RazorMargin)
      {
        Value razor = qsearch<NT>(pos, ss, alpha, beta);
        //mcts Cardanobile from joergoster begin
        if(perceptronSearch)
          {
            thisThread->visits++;
            thisThread->allScores += (ss->ply % 2 == 0) ? razor : -razor;

          }
        //mcts Cardanobile from joergoster end
        return razor;


      }

    improving =   ss->staticEval >= (ss-2)->staticEval
               || (ss-2)->staticEval == VALUE_NONE;

    // Step 8. Futility pruning: child node (~30 Elo)
    if (   !PvNode
        &&  depth < 7 * ONE_PLY
        &&  eval - futility_margin(depth, improving) >= beta
        &&  eval < VALUE_KNOWN_WIN) // Do not return unproven wins
      //mcts Cardanobile from joergoster begin
      {
	if(perceptronSearch)
	  {
	    thisThread->visits++;
	    thisThread->allScores += (ss->ply % 2 == 0) ? eval : -eval;
	  }
	return eval;
      }
      //mcts Cardanobile from joergoster end
    // Step 9. Null move search with verification search (~40 Elo)
    if (   !PvNode
        && (ss-1)->currentMove != MOVE_NULL
        && (ss-1)->statScore < 23200
        &&  eval >= beta
        &&  pureStaticEval >= beta - int(320 * log(depth / ONE_PLY)) + 500 //from Corchess
        && !excludedMove
        &&  thisThread->selDepth + 5 > thisThread->rootDepth / ONE_PLY //from Corchess
        &&  pos.non_pawn_material(us) > BishopValueMg //from Corchess
        && (ss->ply >= thisThread->nmpMinPly || us != thisThread->nmpColor)
	    && ((pos.this_thread()->shashinQuiescentCapablancaMaxScore) ||
		(((abs(eval) < 2 * VALUE_KNOWN_WIN ) && !(depth > 4 * ONE_PLY && (MoveList<LEGAL, KING>(pos).size() < 1 || MoveList<LEGAL>(pos).size() < 6)))))//from MateFinder
    )
    {
        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth and value
        Depth R = std::max(1, int(2.6 * log(depth / ONE_PLY)) + std::min(int(eval - beta) / 200, 3)) * ONE_PLY; //from Corchess

        ss->currentMove = MOVE_NULL;
        ss->continuationHistory = &thisThread->continuationHistory[NO_PIECE][0];

        pos.do_null_move(st);

        Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode);

        pos.undo_null_move();

        if (nullValue >= beta)
        {
            // Do not return unproven mate scores
            if (nullValue >= VALUE_MATE_IN_MAX_PLY)
                nullValue = beta;

            if (thisThread->nmpMinPly || (abs(beta) < VALUE_KNOWN_WIN && depth < 12 * ONE_PLY))
              //mcts Cardanobile from joergoster begin
              {
    	    	if(perceptronSearch)
    	      	 {
                    thisThread->visits++;
                    thisThread->allScores += (ss->ply % 2 == 0) ? nullValue : -nullValue;
    	      	 }
                return nullValue;
              }
            //mcts Cardanobile from joergoster end
            assert(!thisThread->nmpMinPly); // Recursive verification is not allowed
            // Do verification search at high depths, with null move pruning disabled
            // for us, until ply exceeds nmpMinPly.
            thisThread->nmpMinPly = ss->ply + 3 * (depth-R) / 4;
            thisThread->nmpColor = us;
            Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false);

            thisThread->nmpMinPly = 0;

            if (v >= beta)
              //mcts Cardanobile from joergoster begin
              {
        	if(perceptronSearch)
        	  {
                    thisThread->visits++;
                    thisThread->allScores += (ss->ply % 2 == 0) ? nullValue : -nullValue;
        	  }
        	return nullValue;
              }
              //mcts Cardanobile from joergoster end
        }
    }

    // Step 10. ProbCut (~10 Elo)
    // If we have a good enough capture and a reduced search returns a value
    // much above beta, we can (almost) safely prune the previous move.
    if (   !PvNode
        &&  depth >= 5 * ONE_PLY
        &&  abs(beta) < VALUE_MATE_IN_MAX_PLY)
    {
        Value raisedBeta = std::min(beta + 216 - 48 * improving, VALUE_INFINITE);
        MovePicker mp(pos, ttMove, raisedBeta - ss->staticEval, &thisThread->captureHistory);
        int probCutCount = 0;

        while (  (move = mp.next_move()) != MOVE_NONE
               && probCutCount < 2 + 2 * cutNode)
            if (move != excludedMove && pos.legal(move))
            {
                probCutCount++;

                ss->currentMove = move;
                ss->continuationHistory = &thisThread->continuationHistory[pos.moved_piece(move)][to_sq(move)];

                assert(depth >= 5 * ONE_PLY);

                pos.do_move(move, st);

                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss+1, -raisedBeta, -raisedBeta+1);

                // If the qsearch held, perform the regular search
                if (value >= raisedBeta)
                    value = -search<NonPV>(pos, ss+1, -raisedBeta, -raisedBeta+1, depth - 4 * ONE_PLY, !cutNode);

                pos.undo_move(move);

                if (value >= raisedBeta)
                  //mcts Cardanobile from joergoster begin
                  {
                    if(perceptronSearch)
                      {
                        thisThread->visits++;
                        thisThread->allScores += (ss->ply % 2 == 0) ? value : -value;
                      }
                    return value;
                  }
		  //mcts Cardanobile from joergoster end
            }
    }

    // Step 11. Internal iterative deepening (~2 Elo)
    if (depth >= 8 * ONE_PLY && !ttMove)
    {
        search<NT>(pos, ss, alpha, beta, depth - 7 * ONE_PLY, cutNode);

        tte = TT.probe(posKey, ttHit);
        ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;
        ttMove = ttHit ? tte->move() : MOVE_NONE;
    }

moves_loop: // When in check, search starts from here

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr, (ss-4)->continuationHistory,
                                          nullptr, (ss-6)->continuationHistory };

    Move countermove = thisThread->counterMoves[pos.piece_on(prevSq)][prevSq];

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers);

    value = bestValue; // Workaround a bogus 'uninitialized' warning under gcc
    moveCountPruning = false;
    ttCapture = ttMove && pos.capture_or_promotion(ttMove);

    // Step 12. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;

      ss->moveCount = ++moveCount;

      if (rootNode && thisThread == Threads.main() && Time.elapsed() > 3000)
          sync_cout << "info depth " << depth / ONE_PLY
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = DEPTH_ZERO;
      captureOrPromotion = pos.capture_or_promotion(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);

      // Step 13. Extensions (~70 Elo)

      // Singular extension search (~60 Elo). If all moves but one fail low on a
      // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
      // then that move is singular and should be extended. To verify this we do
      // a reduced search on all the other moves but the ttMove and if the
      // result is lower than ttValue minus a margin then we will extend the ttMove.

      //kellykyniama mcts begin
      if (persistedSelfLearning && minSons == 1 && move == expttMove
      	  && pos.legal(move) && visits > 6
      	  )
      {
	  SE = true;
      }
      //kellykyniama mcts end

      if (    depth >= 8 * ONE_PLY
          &&  move == ttMove
          && !rootNode
          && !excludedMove // Avoid recursive singular search
      /*  &&  ttValue != VALUE_NONE Already implicit in the next condition */
          &&  abs(ttValue) < VALUE_KNOWN_WIN
          && (tte->bound() & BOUND_LOWER)
          &&  tte->depth() >= depth - 3 * ONE_PLY
          &&  pos.legal(move))
      {
          Value singularBeta = ttValue - 2 * depth / ONE_PLY;
          ss->excludedMove = move;
          value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, depth / 2, cutNode);
          ss->excludedMove = MOVE_NONE;

          if (value < singularBeta)
              extension = ONE_PLY;

          // Multi-cut pruning
          // Our ttMove is assumed to fail high, and now we failed high also on a reduced
          // search without the ttMove. So we assume this expected Cut-node is not singular,
          // that is multiple moves fail high, and we can prune the whole subtree by returning
          // the hard beta bound.
          else if (cutNode && singularBeta > beta)
            //mcts Cardanobile from joergoster begin
            {
              if(perceptronSearch)
        	{
                  thisThread->visits++;
                  thisThread->allScores += (ss->ply % 2 == 0) ? beta : -beta;
        	}
              return beta;
            }
	    //mcts Cardanobile from joergoster end
      }

      // Check extension (~2 Elo)
      else if (    givesCheck
               && (pos.blockers_for_king(~us) & from_sq(move) || pos.see_ge(move)))
          extension = ONE_PLY;

      // Shuffle extension
      else if(pos.rule50_count() > 14 && ss->ply > 14 && depth < 3 * ONE_PLY && PvNode)
          extension = ONE_PLY;

      // Castling extension
      else if (type_of(move) == CASTLING)
          extension = ONE_PLY;

      // Calculate new depth for this move
      newDepth = depth - ONE_PLY + extension;

      // Step 14. Pruning at shallow depth (~170 Elo)
      if ( ((!PvNode) ||(!rootNode && (pos.this_thread()->shashinQuiescentCapablancaMaxScore))) //from MateFinder
          && pos.non_pawn_material(us)
          && bestValue > VALUE_MATED_IN_MAX_PLY)
      {
          // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold
          moveCountPruning = moveCount >= futility_move_count(improving, depth / ONE_PLY);

          if (   !captureOrPromotion
              && !givesCheck
              && !pos.advanced_pawn_push(move))
          {
              // Move count based pruning (~30 Elo)
              if (moveCountPruning)
                  continue;
			  //kellykyniama mcts begin
			  if (persistedSelfLearning && SE && moveCount > 3)
				  continue;
			  //kellykyniama mcts end

              // Reduced depth of the next LMR search
              int lmrDepth = lessPruningMode ? std::max(newDepth - reductionCC<PvNode>(improving, depth, moveCount), DEPTH_ZERO) / ONE_PLY : std::max(newDepth - reduction<PvNode>(improving, depth, moveCount), DEPTH_ZERO) / ONE_PLY;

              // Countermoves based pruning (~20 Elo)
              if (   lmrDepth < 3 + ((ss-1)->statScore > 0 || (ss-1)->moveCount == 1)
                  && (*contHist[0])[movedPiece][to_sq(move)] < CounterMovePruneThreshold
                  && (*contHist[1])[movedPiece][to_sq(move)] < CounterMovePruneThreshold)
                  continue;

              // Futility pruning: parent node (~2 Elo)
              if (   lmrDepth < 7
                  && !inCheck
                  && ss->staticEval + 256 + 200 * lmrDepth <= alpha)
                  continue;

              // Prune moves with negative SEE (~10 Elo)
              if (!pos.see_ge(move, Value(-29 * lmrDepth * lmrDepth)))
                  continue;
          }
          else if (!pos.see_ge(move, -PawnValueEg * (depth / ONE_PLY))) // (~20 Elo)
                  continue;
      }

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!rootNode && !pos.legal(move))
      {
          ss->moveCount = --moveCount;
          continue;
      }

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[movedPiece][to_sq(move)];

      // Step 15. Make the move
      pos.do_move(move, st, givesCheck);
      bool shashinCapablancaPos=pos.this_thread()->shashinValue==SHASHIN_POSITION_CAPABLANCA;//for lmr patches
      // Step 16. Reduced depth search (LMR). If the move fails high it will be
      // re-searched at full depth.
      if (    depth >= 3 * ONE_PLY
          &&  moveCount > 1
          && (!captureOrPromotion || moveCountPruning) 
	  	  && ((pos.this_thread()->shashinQuiescentCapablancaMaxScore) || (thisThread->selDepth > depth //from JEllis MateFinder
	      && !(depth >= 16 * ONE_PLY && ss->ply < 3 * ONE_PLY) //from JEllis MateFinder
	  )
	 )
      )
      {
          Depth r = lessPruningMode ? reductionCC<PvNode>(improving, depth, moveCount) : reduction<PvNode>(improving, depth, moveCount);

          // Decrease reduction if position is or has been on the PV
          if (ttPv)
              r -= ONE_PLY;

          // Decrease reduction if opponent's move count is high (~10 Elo)
          if ((ss-1)->moveCount > 15)
              r -= ONE_PLY;

          if (!captureOrPromotion)
          {
              // Increase reduction if ttMove is a capture (~0 Elo)
              if (ttCapture)
                  r += ONE_PLY;

              // Increase reduction for cut nodes (~5 Elo)
              if (cutNode)
                  r += 2 * ONE_PLY;


              //patch KingMoves MJZ begin 16/02/2019
              // Increase reduction for king moves at MG
	      if (type_of(movedPiece) == KING
		  && pos.non_pawn_material() > 8000
		      && type_of(move) != CASTLING
		      && !inCheck && shashinCapablancaPos )
	      {
		  r += ONE_PLY;
	      }
	      //patch KingMoves MJZ end 16/02/2019
	      //patch lmr_pawnPushVsK 09/02/2019 begin
	      // Less reduction for pawn moves near the king
	      if (   type_of(movedPiece) == PAWN
		  && pos.non_pawn_material(us) > RookValueMg + 2 * KnightValueMg
		  && std::abs(file_of(to_sq(move)) - file_of(pos.square<KING>(~us))) <= 1
		  && std::abs(rank_of(to_sq(move)) - rank_of(pos.square<KING>(~us))) <= 3
		  && !shashinCapablancaPos)
	      {
		  r -= ONE_PLY;
	      }
	      ////patch lmr_pawnPushVsK 09/02/2019 end

              // Decrease reduction for moves that escape a capture. Filter out
              // castling moves, because they are coded as "king captures rook" and
              // hence break make_move(). (~5 Elo)
              else if (    type_of(move) == NORMAL
                       && !pos.see_ge(make_move(to_sq(move), from_sq(move))))
                  r -= 2 * ONE_PLY;
	      //patch lmrpp 11/02/2019 begin
              else if (type_of(movedPiece) == PAWN
                      && relative_rank(us, rank_of(from_sq(move))) > RANK_4
		      && !shashinCapablancaPos)
                  r -= ONE_PLY;
	      //patch lmrpp 11/02/2019 end
	      ss->statScore =  thisThread->mainHistory[us][from_to(move)]
                             + (*contHist[0])[movedPiece][to_sq(move)]
                             + (*contHist[1])[movedPiece][to_sq(move)]
                             + (*contHist[3])[movedPiece][to_sq(move)]
                             - 4000;

              // Decrease/increase reduction by comparing opponent's stat score (~10 Elo)
              if (ss->statScore >= 0 && (ss-1)->statScore < 0)
                  r -= ONE_PLY;

              else if ((ss-1)->statScore >= 0 && ss->statScore < 0)
                  r += ONE_PLY;

              if(perceptronSearch)
              {
		// Infer using a perceptron, 52%
		features[0] = float(abs(bestValue) * pos.non_pawn_material());
		features[1] = float(ss->statScore);
		features[2] = float(moveCount);
		features[3] = float(int(r));
		prediction  = infer(features);
  		trainPerc = true;
                // Decrease/increase reduction for moves with a good/bad history (~30 Elo)
  		if((pos.this_thread()->shashinValue!=SHASHIN_POSITION_TAL)
          	  && (pos.this_thread()->shashinValue!=SHASHIN_POSITION_PETROSIAN))
  		{
  		  r -= (ss->statScore +  2000 * (prediction - 1))/ 20000 * ONE_PLY;
  		}
              }
              else
              {
                  r -= ss->statScore / 20000 * ONE_PLY;
              }
          }
          if(newDepth - r + 8 * ONE_PLY < thisThread->rootDepth){ //from JEllis MateFinder
              r = std::min(r, (Depth)(pos.this_thread()->shashinMaxLmr)); //from Sugar
          }

          Depth d = std::max(newDepth - std::max(r, DEPTH_ZERO), ONE_PLY);

          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true);

          if (trainPerc && perceptronSearch){
             int result = value > alpha;
             if (prediction != result){
                train(features, 1e-2);
             }
             trainPerc = false;
             dbg_hit_on(prediction == result);
          }

          doFullDepthSearch = (value > alpha && d != newDepth);
      }
      else
          doFullDepthSearch = !PvNode || moveCount > 1;

      // Step 17. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth, !cutNode);

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;

          value = -search<PV>(pos, ss+1, -beta, -alpha, newDepth, false);
      }

      // Step 18. Undo move
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 19. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
          return VALUE_ZERO;

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);
          //mcts Cardanobile from joergoster begin
          if(perceptronSearch)
            {
              // Add all visits and returned scores to this root move's stats
              rm.visits += thisThread->visits;
              rm.zScore += thisThread->allScores;

              thisThread->visits = 0;
              thisThread->allScores = 0;
            }
           //mcts Cardanobile from joergoster end

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = value;
              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each
              // iteration. This information is used for time management: When
              // the best move changes frequently, we allocate some more time.
              if (moveCount > 1 && thisThread == Threads.main())
                  ++static_cast<MainThread*>(thisThread)->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
              // is not a problem when sorting because the sort is stable and the
              // move position in the list is preserved - just the PV is pushed up.
              rm.score = -VALUE_INFINITE;
      }

      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
                  alpha = value;
              else
              {
                  assert(value >= beta); // Fail high
                  ss->statScore = 0;
                  break;
              }
          }
      }

      if (move != bestMove)
      {
          if (captureOrPromotion && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!captureOrPromotion && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 20. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha
                   :     inCheck ? mated_in(ss->ply) : VALUE_DRAW;
    else if (bestMove)
    {
        // Quiet best move: update move sorting heuristics
        if (!pos.capture_or_promotion(bestMove))
            update_quiet_stats(pos, ss, bestMove, quietsSearched, quietCount,
                               stat_bonus(depth + (bestValue > beta + PawnValueMg ? ONE_PLY : DEPTH_ZERO)));

        update_capture_stats(pos, bestMove, capturesSearched, captureCount, stat_bonus(depth + ONE_PLY));

        // Extra penalty for a quiet TT or main killer move in previous ply when it gets refuted
        if (   ((ss-1)->moveCount == 1 || ((ss-1)->currentMove == (ss-1)->killers[0]))
            && !pos.captured_piece())
                update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + ONE_PLY));

    }
    // Bonus for prior countermove that caused the fail low
    else if (   (depth >= 3 * ONE_PLY || PvNode)
             && !pos.captured_piece())
        update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth));

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    if (!excludedMove)
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, pureStaticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);
    //mcts Cardanobile from joergoster begin
	if(perceptronSearch)
      {
	  thisThread->visits++;
	  thisThread->allScores += (ss->ply % 2 == 0) ? bestValue : -bestValue;
      }
	//mcts Cardanobile from joergoster end
    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main
  // search function with depth zero, or recursively with depth less than ONE_PLY.
  template <NodeType NT>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    constexpr bool PvNode = NT == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= DEPTH_ZERO);
    assert(depth / ONE_PLY * ONE_PLY == depth);

    Move pv[MAX_PLY+1];
    StateInfo st;
    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Value bestValue, value, ttValue, futilityValue, futilityBase, oldAlpha;
    bool ttHit, pvHit, inCheck, givesCheck, evasionPrunable;
    int moveCount;

    if (PvNode)
    {
        oldAlpha = alpha; // To flag BOUND_EXACT when eval above alpha and no available moves
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    (ss+1)->ply = ss->ply + 1;
    ss->currentMove = bestMove = MOVE_NONE;
    ss->continuationHistory = &thisThread->continuationHistory[NO_PIECE][0];
    inCheck = pos.checkers();
    moveCount = 0;

    // Check for an immediate draw or maximum ply reached
    if (   pos.is_draw(ss->ply)
        || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !inCheck) ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ttHit);
    ttValue = ttHit ? value_from_tt(tte->value(), ss->ply) : VALUE_NONE;
    ttMove = ttHit ? tte->move() : MOVE_NONE;
    pvHit = ttHit && tte->is_pv();

    if (  !PvNode
        && ttHit
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (ttValue >= beta ? (tte->bound() & BOUND_LOWER)
                            : (tte->bound() & BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    if (inCheck)
    {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ttHit)
        {
            // Never assume anything on values stored in TT
            if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                ss->staticEval = bestValue = evaluate(pos);

            // Can ttValue be used as a better position evaluation?
            if (    ttValue != VALUE_NONE
                && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
            ss->staticEval = bestValue =
            (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                             : -(ss-1)->staticEval + 2 * Eval::Tempo;

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 128;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr, (ss-4)->continuationHistory,
                                          nullptr, (ss-6)->continuationHistory };

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions and checks (only if depth >= DEPTH_QS_CHECKS) will
    // be generated.
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      to_sq((ss-1)->currentMove));

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      givesCheck = pos.gives_check(move);

      moveCount++;

      // Futility pruning
      if (   !inCheck
          && !givesCheck
          &&  futilityBase > -VALUE_KNOWN_WIN
          && !pos.advanced_pawn_push(move))
      {
          assert(type_of(move) != ENPASSANT); // Due to !pos.advanced_pawn_push

          futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

          if (futilityValue <= alpha)
          {
              bestValue = std::max(bestValue, futilityValue);
              continue;
          }

          if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
          {
              bestValue = std::max(bestValue, futilityBase);
              continue;
          }
      }

      // Detect non-capture evasions that are candidates to be pruned
      evasionPrunable =    inCheck
                       &&  (depth != DEPTH_ZERO || moveCount > 2)
                       &&  bestValue > VALUE_MATED_IN_MAX_PLY
                       && !pos.capture(move);

      // Don't search moves with negative SEE values
      if (  (!inCheck || evasionPrunable)
          && !pos.see_ge(move))
          continue;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Check for legality just before making the move
      if (!pos.legal(move))
      {
          moveCount--;
          continue;
      }

      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[pos.moved_piece(move)][to_sq(move)];

      // Make and search the move
      pos.do_move(move, st, givesCheck);
      value = -qsearch<NT>(pos, ss+1, -beta, -alpha, depth - ONE_PLY);
      pos.undo_move(move);

      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
              else
                  break; // Fail high
          }
       }
    }
	//from Sugar
    if (variety && (bestValue + (variety * PawnValueEg / 100) >= 0 ))
	  bestValue += rand() % (variety + 1);
    //end from Sugar
	// All legal moves have been searched. A special case: If we're in check
    // and no legal moves were found, it is checkmate.
    if (inCheck && bestValue == -VALUE_INFINITE)
        return mated_in(ss->ply); // Plies to mate from the root

    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER :
              PvNode && bestValue > oldAlpha  ? BOUND_EXACT : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate score from "plies to mate from the root" to
  // "plies to mate from the current position". Non-mate scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_MATE_IN_MAX_PLY  ? v + ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): It adjusts a mate score
  // from the transposition table (which refers to the plies to mate/be mated
  // from current position) to "plies to mate/be mated from the root".

  Value value_from_tt(Value v, int ply) {

    return  v == VALUE_NONE             ? VALUE_NONE
          : v >= VALUE_MATE_IN_MAX_PLY  ? v - ply
          : v <= VALUE_MATED_IN_MAX_PLY ? v + ply : v;
  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, and -4 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4, 6})
        if (is_ok((ss-i)->currentMove))
            (*(ss-i)->continuationHistory)[pc][to] << bonus;
  }


  // update_capture_stats() updates move sorting heuristics when a new capture best move is found

  void update_capture_stats(const Position& pos, Move move,
                            Move* captures, int captureCount, int bonus) {

      CapturePieceToHistory& captureHistory =  pos.this_thread()->captureHistory;
      Piece moved_piece = pos.moved_piece(move);
      PieceType captured = type_of(pos.piece_on(to_sq(move)));

      if (pos.capture_or_promotion(move))
          captureHistory[moved_piece][to_sq(move)][captured] << bonus;

      // Decrease all the other played capture moves
      for (int i = 0; i < captureCount; ++i)
      {
          moved_piece = pos.moved_piece(captures[i]);
          captured = type_of(pos.piece_on(to_sq(captures[i])));
          captureHistory[moved_piece][to_sq(captures[i])][captured] << -bonus;
      }
  }


  // update_quiet_stats() updates move sorting heuristics when a new quiet best move is found

  void update_quiet_stats(const Position& pos, Stack* ss, Move move,
                          Move* quiets, int quietCount, int bonus) {

    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][from_to(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }

    // Decrease all the other played quiet moves
    for (int i = 0; i < quietCount; ++i)
    {
        thisThread->mainHistory[us][from_to(quiets[i])] << -bonus;
        update_continuation_histories(ss, pos.moved_piece(quiets[i]), to_sq(quiets[i]), -bonus);
    }
  }

  // When playing with strength handicap, choose best move among a set of RootMoves
  // using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.

  Move Skill::pick_best(size_t multiPV) {

    const RootMoves& rootMoves = Threads.main()->rootMoves;
    static PRNG rng(now()); // PRNG sequence should be non-deterministic

    // RootMoves are already sorted by score in descending order
    Value topScore = rootMoves[0].score;
    int delta = std::min(topScore - rootMoves[multiPV - 1].score, PawnValueMg);
    int weakness = 120 - 2 * level;
    int maxScore = -VALUE_INFINITE;

    // Choose best move. For each move score we add two terms, both dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one is
    // random. Then we choose the move with the resulting highest score.
    for (size_t i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = (  weakness * int(topScore - rootMoves[i].score)
                    + delta * (rng.rand<unsigned>() % weakness)) / 128;

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best = rootMoves[i].pv[0];
        }
    }

    return best;
  }

} // namespace

/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint lastInfoTime = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tick = Limits.startTime + elapsed;

  if (tick - lastInfoTime >= 1000)
  {
      lastInfoTime = tick;
      dbg_print();
  }

  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();
  uint64_t tbHits = Threads.tb_hits() + (TB::RootInTB ? rootMoves.size() : 0);

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = (i <= pvIdx && rootMoves[i].score != -VALUE_INFINITE);

      if (depth == ONE_PLY && !updated)
          continue;

      Depth d = updated ? depth : depth - ONE_PLY;
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;

      bool tb = TB::RootInTB && abs(v) < VALUE_MATE - MAX_PLY;
      v = tb ? rootMoves[i].tbScore : v;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";
      //updateShashinValues(pos.this_thread(),(Value)(v * 100 / PawnValueEg));//from Shashin
      ss << "info"
         << " depth "    << d / ONE_PLY
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v);

      if (!tb && i == pvIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " tbhits "   << tbHits
         << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

void Tablebases::rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {

    RootInTB = false;
    UseRule50 = SYZ_50_MOVE;//from Shashin
    ProbeDepth = int(Options["SyzygyProbeDepth"]) * ONE_PLY;
    Cardinality = int(Options["SyzygyProbeLimit"]);
    bool dtz_available = true;

    // Tables with fewer pieces than SyzygyProbeLimit are searched with
    // ProbeDepth == DEPTH_ZERO
    if (Cardinality > MaxCardinality)
    {
        Cardinality = MaxCardinality;
        ProbeDepth = DEPTH_ZERO;
    }

    if (Cardinality >= popcount(pos.pieces()) && !pos.can_castle(ANY_CASTLING))
    {
        // Rank moves using DTZ tables
        RootInTB = root_probe(pos, rootMoves);

        if (!RootInTB)
        {
            // DTZ tables are missing; try to rank moves using WDL tables
            dtz_available = false;
            RootInTB = root_probe_wdl(pos, rootMoves);
        }
    }

    if (RootInTB)
    {
        // Sort moves according to TB rank
        std::sort(rootMoves.begin(), rootMoves.end(),
                  [](const RootMove &a, const RootMove &b) { return a.tbRank > b.tbRank; } );

        // Probe during search only if DTZ is not available and we are winning
        if (dtz_available || rootMoves[0].tbScore <= VALUE_DRAW)
            Cardinality = 0;
    }
    else
    {
        // Assign the same rank to all moves
        for (auto& m : rootMoves)
            m.tbRank = 0;
    }
}
//from mcts begin
void kelly(bool start)
{
	startpoint = start;
}

void files(int x, Key FileKey)
{
	EXP.new_search();
	useExp = true;
	OpFileKey[x] = FileKey;
	if (FileKey)
	{
		string openings;
		char *opnings;

		std::ostringstream ss;
		ss << FileKey;
		openings = ss.str() + ".bin";
		opnings = new char[openings.length() + 1];
		std::strcpy(opnings, openings.c_str());
		EXPload(opnings);
		openingswritten = x;
	}
}
//end mcts begin
