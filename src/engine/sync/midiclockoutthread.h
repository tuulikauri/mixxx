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
    bool queueDirectRTMidiMultiple(uint8_t status, int count);
    bool sendDirectRTMidi(uint8_t status);  ///< Sends the next byte of MIDI data and removes it from midiFIFOQueue
    void setMidiClockOutController(Controller* pMidiClockOutController);
    void deleteMidiClockOutController();

    // Beat clock
    double getBeatSpeedFromBpm(double bpm);
    double getBeatDistanceAt(std::chrono::steady_clock::time_point time);
    double setBeatTimerParameters(std::chrono::steady_clock::time_point startTime, double bpm);
    double setBeatPosAtTime(std::chrono::steady_clock::time_point time, double beatPos, bool addExisting = true);
    void setBeatClockState(bool state);
    bool getBeatClockState();
    double getNextTickBeatDistance(double beatDistance);
    double getPrevTickBeatDistance(double beatDistance);
    int32_t getTicksBetween(double beatDistanceStart, double beatDistanceEnd);
    double resetBeatTimerPos(std::chrono::steady_clock::time_point time);
    double syncAdjustment(std::chrono::steady_clock::time_point time, double syncShift);   

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
    void tickSent();    

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
    double m_nextBeat;
    std::chrono::steady_clock::time_point m_beatStartTime;
    bool m_beatClockRunning;
    int32_t m_beatCount;


    
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