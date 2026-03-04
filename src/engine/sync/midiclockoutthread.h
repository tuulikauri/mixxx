#pragma once

#include <QMutex>
#include <QThread>
#include "util/fifo.h"
#include <QWaitCondition> //TODO(Tuuli): do we want this too?

#include <chrono>

#include "controllers/controller.h"


// Timestamps in portmidi need a sync latency when the port is opened. Also currently we dont use any buffer
// or sync time, so no timestamps for midi outs. A buffer would make sense to have.
//
// The main audio thread will block to wait for a mutex to free.
// Thread can emit signals for the GUI

/// This thread is based around the concept of a beatSpeed.
/// beatSpeed [ beats per us ] = bpm [b/min] * 1/60 [min/s] * 1/1,000,000 [s/us] = bpm / 60,000,000 [b/us]
/// sync events : change the startPos and either send or skip sync ticks to sync external devices
/// tempo events : change the beatSpeed, at a specific startTime
///
/// beatPosAt(time) = startPos + beatSpeed * (time - startTime)
/// nextBeat = 2
/// if (beatPosTotal > nextBeat){
///     tick();
///     nextBeat++;
/// }
/// 
// Since this is a tempo-follower it will need to catch any tempo-related events. Within MidiClockOut there needs to be a listener, which then forwards events. If the DJ wants to stop the clock for external sequencers, they should expect clock pulses to stop pretty quick.. What if we treated it as a deck? Doesnt sync lock, matches tempo if synced. Could enable or disable sync follow and just focus on tempo. QElapsedTimer for measuring precise times might be another option.

class MidiClockOut;

class MidiClockOutThread : public QThread {
    Q_OBJECT
public:
    MidiClockOutThread(MidiClockOut* parent = nullptr);
    ~MidiClockOutThread();
    /// @brief called from [main]    
    void startMidiClockOutThread();
    void stopPlease();
    void testuSleepLength();

    /// Midi

    /// @brief Adds status byte to the queue of MIDI data to send; called from [main]
    /// @param status The realtime status byte to send
    /// @details Queues to portMidi device with midi_clock_out script mapped. The pointer m_pMidiClockOutController stores this device controller address
    bool queueDirectRTMidi(uint8_t status); 
    uint16_t queueDirectRTMidiMultiple(uint8_t status, int count); ///< Adds status to the queue of MIDI data to send, count times
    /// @brief Sends 0xF8,FA,FB or FC status byte to portMidi device with midi_clock_out script mapped
    /// @param status byte to send (0xF8,FA,FB or FC only)
    /// @return success or failure
    /// @details Only handles F8,FA,FB,FC, otherwise returns false. This function is used in run(). run() removes the next byte from midiFIFOQueue, checks for a pending sync tick-skip (disgards 0xF8 if found), and then sends the MIDI data using this function
    bool sendDirectRTMidi(uint8_t status); 
    /// @brief called from [main]
    /// @param pMidiClockOutController 
    void setMidiClockOutController(Controller* pMidiClockOutController);
    /// @brief called from [main]
    void deleteMidiClockOutController();

    /// Beat clock

    double calcBeatSpeedFromBpm(double bpm); ///< Utility function to convert bpm to beats per microsecond "beatSpeed"
    int32_t calcTicksFromBeatPos(double beatPosition); ///< Utility function to convert a beatPosition to ticks; does not consider the current beat clock, only the size of beatPosition
    int32_t calcTicksBetween(double beatPositionStart, double beatPositionEnd); ///< Utility function to convert a twoo beatPositions to ticks; does not consider the current beat clock, only the size of beatPositions
    /// @brief Utility function to determine the next following 24PPQN position. If the position is on a tick, returns the next 24 PPQN tick
    /// @param beatPosition to calc from 
    /// @return beatPosition of the next 24PPQN tick
    double calcNextTickBeatPos(double beatPosition); 
    double calcPrevTickBeatPos(double beatPosition); ///< Utility function to determine the previous 24PPQN position. If the position is on a tick, returns the tick and not the previous 24 PPQN tick // TODO(Tuuli): Does this make sense to round this way? Not used, havent thought about it yet...

    /// @brief Enables and disables the clock in run(); called from [main]
    /// @param state 
    void setBeatClockState(bool state);
    bool getBeatClockState();
    

    /// @brief Calculates the beatPos at a point in time
    /// @param time The time_point for the beatPos
    /// @return beatPos, each integer is a full beat (24 ticks) at the current tempo. 
    /// @details Uses the current beat tempo and position; each integer is a full beat (24 ticks) at the current tempo. Note that mixxx's beatDistance is usually only with respect to the previous beat and never more than 1; this is named beatPosition (or beatPos) to distinguish it
    double getBeatPosAt(std::chrono::steady_clock::time_point time); 

    /// @brief called from [main]
    /// @param startTime 
    /// @param bpm 
    /// @return Returns the previous beat position at startTime that the new tempo starts from
    double setBeatTempoAt(std::chrono::steady_clock::time_point startTime, double bpm); 

    /// @brief Moves the start position and start time; called from [main]
    /// @param time The time_point for the beatPos
    /// @details Preserves whole beats in the beat position by default. 
    /// @return Returns how far the jump was and which direction, used to send sync ticks
    double setBeatPosAt(std::chrono::steady_clock::time_point time, double beatPos, bool addExisting = true); 
    
    /// @brief Resets the beatPos to start at zero at a new time; called from [main]
    /// @param time The time_point for the beatPos to equal zero
    /// @return Returns the previous beat position for that time, before reset
    double resetBeatPosAt(std::chrono::steady_clock::time_point time);
    

    /// @brief Returns the next planned beatPos for the next tick, that was set in the previous tick
    /// @return next planned beatPos
    double getNextBeatPos();


    /// @brief The pending sync tick adjustment amount is how far the external sequencer should jump to catch up to a beatjump
    /// @return Pending sync ticks; Negative = skip ticks (slower); positive = extra ticks (faster)
    /// @details
    /// If the error correction is to speed up ie say jump 4 beats fwd, sequencers can only handle data so fast
    /// on din midi. So error correction has a max rate, and the thread reduces the error every time it wakes.
    /// For jumping it might stay awake and send every 320us. 7680us for 24 pulses. 30720us for 24x4 (1 bar 4/4).
    /// 92160us for 3 bars (288 ticks). 3 bars makes sense as the max distance to jump a sequencer fwd.
    /// To stall, simply wait for the duration, or for user input.
    int16_t getPendingSyncAdjustment(); 

    /// @brief Plans a tick sync adjustment, thread reads and sends extra 0xF8 ticks, or skips ticks; called from [main]
    /// @param syncShift; Negative = skip ticks (slower); positive = extra ticks (faster)
    /// @details Limits the pending sync range to 12 beats (3 bars at 4/4, 4 bars at 3/4) (288 ticks)
    void addPendingSyncAdjustment(int16_t syncShift);

    /// @brief called from [main]
    /// @param syncShift; Negative = skip ticks (slower); positive = extra ticks (faster)
    /// @sa getPendingSyncAdjustment
    void addPendingSyncAdjustment(double syncShift);

    /// @brief Clears any pending sync tick edits (extra ticks, or skip-ticks) 
    /// @return the amount removed; Negative = skip ticks (slower); positive = extra ticks (faster)
    int16_t resetPendingSyncAdjustment(); 

    void run() override;

signals:
    /// @brief Signal emitted to change the GUI when a tick is sent to external Midi port. 
    /// @details Advances the bars:beats:sixteenths syncTicks+1 times forward. Should not tick for skipped or failed ticks, so only emit when a tick is sent to external sequencers.
    void tickSent(int32_t syncTicks); 

private:
    QMutex mutex;
    QWaitCondition cond; //TODO(Tuuli) Remove this? Is it helpful / needed?
    MidiClockOut* m_pMidiClockOutParent; //TODO(Tuuli) Remove this? Is it helpful / needed?
    bool stopplz;

    ///Midi
    QMutex midiMutex;
    FIFO<uint8_t>* m_pMidiFIFOQueue; 
    FIFO<uint8_t> m_midiFIFOQueue; //TODO(Tuuli) Only used to create the pointer to it; can this be removed?
    Controller* m_pMidiClockOutController = nullptr; ///< Unowned pointer to the Controller that is linked to "Midi Clock Out" mapping

    /// Beat clock
    QMutex beatMutex;
    bool m_beatClockRunning;
    double m_beatSpeed; ///< beatSpeed [ beats per us ] = bpm [b/min] * 1/60 [min/s] * 1/1,000,000 [s/us] = bpm / 60,000,000 [b/us]
    double m_beatStartPos;  
    std::chrono::steady_clock::time_point m_beatStartTime;    
    double m_nextBeatPos; ///< Next planned beat; updated by the run() function each time it ticks
    int32_t m_tickCount; ///< Counts how many ticks have occurred based on time ONLY. This is for calculating the clock position; it ticks before the message is sent, and does not capture additional or skipped sync ticks (handled by m_tickSyncAdjustment)
    int16_t m_tickSyncAdjustment; ///< Queued ticks to skip or add to queue as extra; negative = skip ticks (slower); positive = extra ticks (faster)
};
