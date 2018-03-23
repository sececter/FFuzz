//===-- ExecutorTimers.cpp ------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Common.h"

#include "klee/CoreStats.h"
#include "klee/Executor.h"
#include "klee/PTree.h"
#include "klee/StatsTracker.h"

#include "klee/ExecutionState.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/System/Time.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"

#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <math.h>

#ifdef __MINGW32__
#include <windows.h>
#endif

using namespace llvm;
using namespace klee;

cl::opt<double>
MaxTime("max-time",
        cl::desc("Halt execution after the specified number of seconds (0=off)"),
        cl::init(0));

///

class HaltTimer : public Executor::Timer {
  Executor *executor;

public:
  HaltTimer(Executor *_executor) : executor(_executor) {}
  ~HaltTimer() {}

  void run() {
    std::cerr << "KLEE: HaltTimer invoked\n";
    executor->setHaltExecution(true);
  }
};

///

static const double kSecondsPerTick = .1;
static volatile unsigned timerTicks = 0;

//S2E: This is to avoid calling expensive time functions on the critical path
//This variable is updated evers second
volatile uint64_t g_timer_ticks = 0;

// XXX hack
extern "C" unsigned dumpStates, dumpPTree;
unsigned dumpStates = 0, dumpPTree = 0;

void Executor::onAlarm(int) {
  ++timerTicks;
}

#ifdef __MINGW32__
VOID CALLBACK TimerProc(
    HWND hwnd,
    UINT uMsg,
    UINT_PTR idEvent,
    DWORD dwTime
)
{
    //XXX: Ugly hack, but there are so many of them anyway
    ++timerTicks;
    //Executor::onAlarm(0);
}
#endif

// oooogalay
void Executor::setupTimersHandler() {
#ifdef __MINGW32__
  HANDLE hTimer;
  SetTimer(0, 0, 1000, TimerProc);
#else
  struct itimerval t;
  struct timeval tv;

  tv.tv_sec = (long) kSecondsPerTick;
  tv.tv_usec = (long) (fmod(kSecondsPerTick, 1.)*1000000);

  t.it_interval = t.it_value = tv;

  ::setitimer(ITIMER_REAL, &t, 0);
  ::signal(SIGALRM, onAlarm);
#endif
}

void Executor::initTimers() {
  static bool first = true;

  if (first) {
    first = false;
    setupTimersHandler();
  }

  if (MaxTime) {
    addTimer(new HaltTimer(this), MaxTime);
  }
}

///

Executor::Timer::Timer() {}

Executor::Timer::~Timer() {}

class Executor::TimerInfo {
public:
  Timer *timer;

  /// Approximate delay per timer firing.
  double rate;
  /// Wall time for next firing.
  double nextFireTime;

public:
  TimerInfo(Timer *_timer, double _rate)
    : timer(_timer),
      rate(_rate),
      nextFireTime(util::getWallTime() + rate) {}
  ~TimerInfo() { delete timer; }
};

void Executor::addTimer(Timer *timer, double rate) {
  timers.push_back(new TimerInfo(timer, rate));
}

void Executor::processTimers(ExecutionState *current,
                             double maxInstTime) {
  static unsigned callsWithoutCheck = 0;
  unsigned ticks = timerTicks;

  ++g_timer_ticks;

  if (!ticks && ++callsWithoutCheck > 1000) {
    setupTimersHandler();
    ticks = 1;
  }

  if (ticks || dumpPTree || dumpStates) {
    if (dumpPTree) {
      char name[32];
      sprintf(name, "ptree%08d.dot", (int) stats::instructions);
      llvm::raw_ostream *os = interpreterHandler->openOutputFile(name);
      if (os) {
        processTree->dump(*os);
        delete os;
      }

      dumpPTree = 0;
    }

    if (dumpStates) {
      llvm::raw_ostream *os = interpreterHandler->openOutputFile("states.txt");

      if (os) {
        for (StateSet::const_iterator it = states.begin(),
               ie = states.end(); it != ie; ++it) {
          ExecutionState *es = *it;
          *os << "(" << es << ",";
          *os << "[";
          ExecutionState::stack_ty::iterator next = es->stack.begin();
          ++next;
          for (ExecutionState::stack_ty::iterator sfIt = es->stack.begin(),
                 sf_ie = es->stack.end(); sfIt != sf_ie; ++sfIt) {
            *os << "('" << sfIt->kf->function->getName().str() << "',";
            if (next == es->stack.end()) {
              *os << "" << "), ";
            } else {
              *os << "" << "), ";
              ++next;
            }
          }
          *os << "], ";

          StackFrame &sf = es->stack.back();
          uint64_t md2u = computeMinDistToUncovered(es->pc,
                                                    sf.minDistToUncoveredOnReturn);
          uint64_t cpicnt = sf.callPathNode->statistics.getValue(stats::instructions);

          *os << "{";
          *os << "'depth' : " << es->depth << ", ";
          *os << "'weight' : " << es->weight << ", ";
          *os << "'queryCost' : " << es->queryCost << ", ";
          *os << "'coveredNew' : " << es->coveredNew << ", ";
          *os << "'instsSinceCovNew' : " << es->instsSinceCovNew << ", ";
          *os << "'md2u' : " << md2u << ", ";
          *os << "'CPicnt' : " << cpicnt << ", ";
          *os << "}";
          *os << ")\n";
        }

        delete os;
      }

      dumpStates = 0;
    }

    if (maxInstTime>0 && current && !removedStates.count(current)) {
      if (timerTicks*kSecondsPerTick > maxInstTime) {
        klee_warning("max-instruction-time exceeded: %.2fs",
                     timerTicks*kSecondsPerTick);
        terminateStateEarly(*current, "max-instruction-time exceeded");
      }
    }

    if (!timers.empty()) {
      double time = util::getWallTime();

      for (std::vector<TimerInfo*>::iterator it = timers.begin(),
             ie = timers.end(); it != ie; ++it) {
        TimerInfo *ti = *it;

        if (time >= ti->nextFireTime) {
          ti->timer->run();
          ti->nextFireTime = time + ti->rate;
        }
      }
    }

    timerTicks = 0;
    callsWithoutCheck = 0;
  }
}

