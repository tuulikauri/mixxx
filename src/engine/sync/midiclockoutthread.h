#pragma once

#include <QMutex>
#include <QThread>
#include "util/fifo.h"
#include <QWaitCondition> //TODO(Tuuli): do we want this too?

#include <chrono>

#include "controllers/controller.h"


/// @brief Read timestamps and send on-time to a portmidi output device. Sleep. Wake up, repeat.
//
// Timestamps in portmidi need a sync latency when the port is opened. Also currently we dont use any buffer
// or sync time, so no timestamps for midi outs. A buffer would make sense to have.

// Thread-sleep-based timer
// [otherthread] thread.restart();
// thread::restart(){
//  mutex.lock();
//  restart = true;
//  mutex.unlock();
// }
// 
// while(true) {
//  sleep(300us);
//  QDebug << now();
//  mutex.lock();
//  if (restart) // same as nsTimer.restart()
//    m_desiredtimestamp = now() + interval;
//    restart = false;
//  if(now() > m_desiredtimestamp-100us) // same as onTimeout slot
//    m_pMidiClockOut->tick; // can this be called from the other thread? probably not..
//    m_pMidiClockOut->getLatestUpdate(); // interval, start time at that BPM
//    m_flag_tick = flag_tick;
//  mutex.unlock();
//  if (m_flag_tick)
//    midiController.sendShort(0xF8,(uint8_t)0x00,(uint8_t)0x00);  
//    m_flag_tick = false;
// }

// The main audio thread will block to wait for a mutex to free.
// Thread can emit signals for the GUI

// QElapsedTimer for measuring precise times
// Add a test for how long sleep(200us) actually sleeps..

class MidiClockOut;

class MidiClockOutThread : public QThread {
    Q_OBJECT
public:
    MidiClockOutThread(MidiClockOut* parent = nullptr);
    ~MidiClockOutThread();
    void stopPlease();
    void startMidiClockOutThread();

    // Midi
    bool queueDirectRTMidi(uint8_t status); ///< Adds status to the queue of MIDI data to send
    bool queueDirectRTMidiMultiple(uint8_t status, int count); ///< Adds status to the queue of MIDI data to send, count times
    bool sendDirectRTMidi(uint8_t status); ///< Sends the next byte of MIDI data and removes it from midiFIFOQueue
    void setMidiClockOutController(Controller* pMidiClockOutController);
    void deleteMidiClockOutController();

    // Beat clock

    double getBeatSpeedFromBpm(double bpm); ///< Utility function to convert bpm to beats per microsecond
    int32_t getTicksFromBeatPos(double beatPosition); ///< Utility function to convert a beatPosition to ticks; does not consider the current beat clock, only the size of beatPosition
    int32_t getTicksBetween(double beatPositionStart, double beatPositionEnd); ///< Utility function to convert a twoo beatPositions to ticks; does not consider the current beat clock, only the size of beatPositions
    double getNextTickBeatPos(double beatPosition); ///< Utility function to determine the next following 24PPQN position. If the position is on a tick, returns the next 24 PPQN tick
    double getPrevTickBeatPos(double beatPosition); ///< Utility function to determine the previous 24PPQN position. If the position is on a tick, returns the tick and not the previous 24 PPQN tick // TODO(Tuuli): Does this make sense to round this way?    

    void setBeatClockState(bool state); ///< Enables and disables the clock in run()
    bool getBeatClockState();
    
    double getBeatPosAt(std::chrono::steady_clock::time_point time); ///< Uses the current beat tempo and position; each integer part is a full beat (24 ticks) at the current tempo. Note that mixxx's beatDistance is usually only with respect to the previous beat and never more than 1; this is named beatPosition to distinguish it
    double setBeatTempoAt(std::chrono::steady_clock::time_point startTime, double bpm); ///< Returns the beat position at startTime
    double setBeatPosAt(std::chrono::steady_clock::time_point time, double beatPos, bool addExisting = true); ///< Moves the start position and start time. Preserves whole beats in the beat position by default. Returns how far the jump was to allow sync shifts            
    double resetBeatPosAt(std::chrono::steady_clock::time_point time);
    
    double getNextBeat(); ///< 

    int16_t getPendingSyncAdjustment(); ///< Negative = skip ticks (slower); positive = extra ticks (faster)
    void addPendingSyncAdjustment(double syncShift);
    void addPendingSyncAdjustment(int16_t syncShift);
    int16_t resetPendingSyncAdjustment(); ///< Clears any pending sync tick edits (extra ticks, or skip-ticks); returns the amount removed

    void run() override;

    /*
    void setTimerInterval(std::chrono::nanoseconds interval);
    void setTickTimerParameters(std::chrono::steady_clock::time_point timeTempoSet, std::chrono::microseconds tempoInterval);
    
    void setTickAdjustment(int16_t adjustment);   ///< Plans a tick sync adjustment; thread reads and sends extra ticks, or skips ticks
    void resetTickAdjustment(); ///< Resets planned sync (extra or skipped) ticks to zero.

    void startMidiClockOutThread();
    void startTicks();
    void stopTicks();

    void sendMidiClockTick();     ///< Queues 0xF8 to portMidi device with midi_clock_out script mapped
    void sendMidiClockStart();    ///< Queues 0xFA to portMidi device with midi_clock_out script mapped
    void sendMidiClockContinue(); ///< Queues 0xFB to portMidi device with midi_clock_out script mapped
    void sendMidiClockStop();     ///< Queues 0xFC to portMidi device with midi_clock_out script mapped    

    void sendMidiClockTTick();     ///< Sends 0xF8 to portMidi device with midi_clock_out script mapped
    void sendMidiClockTStart();    ///< Sends 0xFA to portMidi device with midi_clock_out script mapped
    void sendMidiClockTContinue(); ///< Sends 0xFB to portMidi device with midi_clock_out script mapped
    void sendMidiClockTStop();     ///< Sends 0xFC to portMidi device with midi_clock_out script mapped


    void run() override;
    
   */ 
signals:
    void tickSent(int32_t syncTicks); ///< Emitted to change the GUI. Advances the bars:beats:sixteenths syncTicks+1 times forward. Should not tick for skipped or failed ticks, so only emit when a tick is sent to external sequencers.

//public slots:
    // None; the timer is edited directly, with a Mutex lock for the cross-threading.
private:
    QMutex mutex;
    QWaitCondition cond;
    MidiClockOut* m_pMidiClockOutParent;
    bool stopplz;

    //Midi
    QMutex midiMutex;
    FIFO<uint8_t>* m_pMidiFIFOQueue; 
    FIFO<uint8_t> m_midiFIFOQueue;
    Controller* m_pMidiClockOutController = nullptr; ///< Duplicate unowned pointer to the Controller that is linked to Midi Clock Out mapping

    // Beat clock
    QMutex beatMutex;
    double m_beatSpeed;
    double m_beatStartPos;
    double m_nextBeatPos;
    std::chrono::steady_clock::time_point m_beatStartTime;
    bool m_beatClockRunning;
    int32_t m_tickCount; ///< Counts how many ticks have occurred based on time ONLY. This is for calculating the clock position; it ticks before the message is sent, and does not capture additional or skipped sync ticks (handled by m_tickSyncAdjustment)
    int16_t m_tickSyncAdjustment; ///< Queued ticks to skip or add to queue as extra; negative = skip ticks (slower); positive = extra ticks (faster)

    
    /*
    * OLDDDDDDDDDDDDDDDDDDDDDDDDDDDD
    * 
    bool mV_sendTicksEnabled;

    void testTickLength(); ///< Debug test function

    //std::chrono::steady_clock::time_point m_startTime;
    //std::chrono::nanoseconds m_interval;

    //std::chrono::steady_clock::time_point mV_timeTempoChanged;
    //std::chrono::microseconds mV_interval; 

    //uint32_t m_tickCounter;
    
    int16_t mV_tickAdjustment; ///< Number of ticks to skip or spam to beatjump or otherwise adjust position on external sequencers; multithreaded   
    uint32_t mV_ticksSinceBpmChange;
    std::chrono::microseconds mV_currentTickLength;
    std::chrono::steady_clock::time_point mV_timeReceivedNewLeaderBpm;
    std::chrono::steady_clock::time_point mV_plannedNextTickTime;
    
    bool mV_tickFlag, mV_startFlag, mV_continueFlag, mV_stopFlag;    
    */

};
/* 
* Old draft...
    //midiCLockTimerThread state - is this needed?
    enum threadState {
          waiting,
          processing,
          numberOfStates
    };
    
    struct clockTimes {
        std::chrono::time_point<std::chrono::steady_clock> plannedTime; /// Timestamp as planned, microseconds accuracy
        std::chrono::time_point<std::chrono::steady_clock> sentTime;    /// Timestamp as actually sent, microseconds accuracy
        uint8_t state; /// Meta info about the timestamps status. Sent, upnext, future 1, future 2, abort
    };

    void updateTimestamps(); ///

  signals:
    void tick(); /// Emitted when a tick is sent to portmidi - maybe this would be handy? It will be slowed by the event loop/queue

  public slots:
    // None; the timestamps are edited directly, with a Mutex lock for the cross-threading.
  
  private:
    // Thread that sends clock works with a three-event clock buffer. Stale, currentlyNext (sleeping until
    // this time), nextAfter. Muted locks for bidirectional comms.
    //
    // ClockThread sets flags when it has read the time to send to portmidi, with a readwrite lock. It could
    // have another when its queued the event with portmidi and a timestamp, but that seems excessive. Do mutexes
    // pause and wait if theyre locked, probably?
    //
    // Midiclockout interfaces with and plans all the timesteps. If it tries to edit a timestep that is flagged
    // as already sent, it locks the next one and replans the next timesteps to adjust them instead. Or it can
    // write to an error correction variable, which clockthread reads when it wakes up, before reading the next timesteps.
    //
    // Clockthread reads the timestep list and the current time. Sets a flag that its sleeping for that timestep.
    // It then sets a sleep timer until the next timesteps arrives. When it wakes up it checks if an abort flag
    // is set, if not it sends to portmidi as planned. If its been aborted, it sets an acknowledge flag, doesnt
    // send, reads the error correction value, maybe writes the time that it started error correcting. Then it
    // reads the next timestamp, current time, and sleeps.

    QMutex m_midiTimerMutex; /// m_timeSequenceList are read in this thread but written in MidiClockOut class.    
    clockTimes m_timeSequenceList[4]; /// Circular buffer? Lists the just-sent, next-up, and two future timestamps?    
    std::chrono::microseconds m_currentError; /// Error, edited to adjust the times to send pulses... read to correct next pulse time
    // Do we want a timer, or a fast-loop that just checks now() against plannedTime, or a usleep thread pause?
    QChronoTimer m_midiClockTimer; /// Timer that takes into account current time, m_currentError, and plannedTime

    void testAccumulatedLag(int number); /// Run a short timer @param number times and /
            /// compare to expected total duration (could do a comparison of usleep() /
            /// vs QChronoTimer's results)
    
    void sendNextClock();                /// Locks Mutex, Reads m_timeSequenceList's /
            /// latest upnext time_point, reads m_currentError, changes the state to /
            /// Sent, sets sentTime to the actual time sent, adds to m_currentError if / 
            /// needed, reads the Future 1, changes state to upnext, unlocks the mutex, /
            /// and starts the timer
    
    void updateTimeSequenceList(clockTimes* pNewTimeSequenceList); /// New time sequence info to assign

    // If the error correction is to speed up ie say jump 4 beats fwd, sequencers can only handle data so fast
    // on din midi. So error correction has a max rate, and clockThread reduces the error every time it wakes.
    // For jumping it might stay awake and send every 320us. 7680us for 24 pulses. 30720us for 24x4 (1 bar 4/4).
    // 61440 for 2 bars (192 ticks). 2 bars makes sense as the max distance to jump a sequencer fwd.
    // To stall, simply wait for the duration, or for user input. Wait in intervals of 1ms and check for input.
    void beatJump(int16_t ticksToJump); /// Sends a bunch of pulses, or delays a bunch of     
            /// time, to move the phase 24 ppqn and up to 2 bars (192 ticks).
};
 */
 
//Since this is a tempo-follower it will need to catch any tempo-related events. So somewhere in
//MidiClockOut there needs to be a listener. Or, if it can read the tempo and position on wakeup that 
//would work too, just need to see what is needed and if its available. Ill take a look how youve gone 
//about it for AbletonLink, and the Syncable class.
//
//My initial thought for usleep() is perhaps it wakes with enough time before the next tick to request 
//the data it needs and receive it and process it, ie for any bpm changes, jumps, stop or reset commands. 
//It can set a QTimer for the short duration until the next tick if theres no changes; and then recalculate
//once it has all the data, which will be before the previously planned tick. Then when the short QTimer fires,
//tick the clock, and go back to sleep. If the DJ wants to stop the clock for external sequencers, they 
//should expect clock pulses to stop pretty quick..
//This might loop 12ms usleep-0.5ms QTimer and data update.
//
//Or we just wake up for the previously planned tick, check beatDistance and if it aligns all good. Then 
//plan the next tick based on beatDistance instead of trying to track the DJs tempo (or beat gridded adjustable tempo..)
//
//Ive got a mock GUI and bars:beats:16ths "timer" together, Ive pushed it to my branch here.
//
//What if we treated it as a deck? Doesnt sync lock, matches tempo if synced.
//Could enable or disable sync follow and just focus on tempo