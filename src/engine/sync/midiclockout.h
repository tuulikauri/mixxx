#pragma once
#include <ableton/platforms/stl/Clock.hpp>
#include "engine/sync/midiclockoutthread.h"


#include <chrono>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0) 
#include <QChronoTimer> 
//using QChronoTimerType = QChronoTimer; 
using timerDurationType = std::chrono::nanoseconds;
#else 
#include <QTimer> 
//using QChronoTimerType = QTimer; 
using timerDurationType = std::chrono::milliseconds;
#endif

#include "control/controlpushbutton.h"
#include "engine/channels/enginechannel.h"
#include "engine/enginebuffer.h"
#include "engine/sync/syncable.h"
#include "engine/sync/synccontrol.h"

#include "controllers/controller.h"
 
/// This class manages a Midi clock output (0xF8)

/// This class manages a Midi clock output (0xF8). It prioritizes maintaining the
/// user-selected beat_distance, so the external sequencers are aligned to the beat
/// grid in Mixxx.
/// Object is initialized in EngineSync constructor
/// Inputs: GUI buttons; sync-leader tempo or sync events
/// Outputs: Clock ticks, start, stop, continue
/// Processing: 
/// Sync events (GUI ENABLE, GUI RESTART, GUI NUDGE) and Syncable::updateLeaderBeatDistance
/// - RESTART sets the beats to 1:1:1 (and keeps playing if its playing) 0xFA, or 0xFC 0xFA
/// - ENABLE starts the clock (and does not reset the beats) 0xFB, or 0xFA
/// - Both align the current time_point with the current beat-position-offset 
/// (through the tick_count), and store the time_point for future. As ticks 
/// progress, that time_point is always referenced together with the tick_count 
/// and the tempo_interval between ticks.
/// - NUDGE commands move the beat-position-offset a small amount, and store it for future. 
/// - beat-position-offset adjustments are done by adjusting the tick_count 
/// and outputting extra 0xF8 ticks, or skipping 0xF8 output.
/// - Syncable::updateLeaderBeatDistance maintains current beat-position offset 
/// to the track by adjusting the tick_count to align with the new beat_distance, 
/// and then issuing a beat-position-offset adjustment.
/// 
/// Tempo events Syncable::updateLeaderBpm, which also causes sync errors
/// - Updates the tempo_interval between ticks
/// - Accounts for the non-realtime nature of receiving tempo changes by adjusting 
/// time_points for ticks to maintain sync-lock. Currently this seems hacky and 
/// excessive, it would be better to request the Leader's beat-position and then 
/// move the beat-position-offset to the offset that was set with the sync GUI.
/// 
/// @sa EngineSync 
/// @sa AbletonLink 
/// @sa MidiClockOutThread
/// @dot
/// digraph {
///  splines=polyline;
///  label = "MidiClockOut Logic";
///  tooltop = "Error-correction logic for MidiClockOut";
///  node[fontsize=10 shape=Mrecord];
///  edge[fontsize=10];
///  LISTENING [tooltip = "not playing, but tracking sync and tempo"];
///  RUNNING [tooltip = "clock is running, 0xF8 ticks"];
/// 
///  LISTENING->RUNNING[label = "enable"];
///  RUNNING->ONBEAT[label = "enable"];
///  RUNNING->LISTENING[label = "disable"];
///  subgraph running_graph {
///    ONBEAT [tooltip = "clock is running onbeat"];
///    OFFBEAT [tooltip = "clock is undesirably offbeat"];
/// 
///    ONBEAT->OFFBEAT[label = "drift" labeltooltip = "clock drift from non-realtime OS performance"];
///    ONBEAT->OFFBEAT[label = "tempo" labeltooltip = "mid-tick tempo change, or short instantaneous tempo change"];
///    ONBEAT->OFFBEAT[label = "beatjump" labeltooltip = "beatjump" ];
///    ONBEAT->OFFBEAT[label = "scratch" labeltooltip = "scratching" ];
/// 
///    OFFBEAT->ONBEAT[label = "drift fix" labeltooltip = "automatic clock drift correction"];
///    OFFBEAT->ONBEAT[label = "beatjump fix" labeltooltip = "automatic clock beatjump correction"];
///    OFFBEAT->PENDINGFIX[label = "auto-request" labeltooltip = "clock requests beatDistance"];
///    PENDINGFIX->ONBEAT[label = "leader" labeltooltip = "leader sends beatDistance"];
///    OFFBEAT->ONBEAT[label = "restart" labeltooltip = "DJ restarts"];
///    OFFBEAT->ONBEAT[label = "nudge" labeltooltip = "DJ nudges phase"];
///  }
/// }
/// @enddot

// TODO(Tuuli): is something like this needed?
//using MixxxClockRef = ableton::platforms::stl::Clock; 

class MidiClockOut : public QObject, public Syncable {
  Q_OBJECT
  public:
    MidiClockOut(const QString& group, EngineSync* pEngineSync);
    ~MidiClockOut() override;
    const QString& getGroup() const override {
        return m_group;
    }
    EngineChannel* getChannel() const override {
        return nullptr;
    }   
    /// Notify a Syncable that their mode has changed. The Syncable must record
    /// this mode and return the latest mode in response to getMode().
    void setSyncMode(SyncMode mode) override;
    /// Notify a Syncable that it is now the only currently-playing syncable.
    void notifyUniquePlaying() override;
    /// Notify a Syncable that they should sync phase.
    void requestSync() override;
    /// Must NEVER return a mode that was not set directly via
    /// notifySyncModeChanged.
    SyncMode getSyncMode() const override;
    /// Only relevant for player Syncables.
    bool isPlaying() const override;
    bool isAudible() const override;
    bool isQuantized() const override;
    /// Gets the current speed of the syncable in bpm (bpm * rate slider), doesn't
    /// include scratch or FF/REW values.
    mixxx::Bpm getBpm() const override;      
    /// Gets the beat distance as a fraction from 0 to 1
    double getBeatDistance() const override;
    /// Gets the speed of the syncable if it was playing at 1.0 rate.
    mixxx::Bpm getBaseBpm() const override;
    /// The following functions are used to tell syncables about the state of the
    /// current Sync Master.
    /// Must never result in a call to
    /// SyncableListener::notifyBeatDistanceChanged or signal loops could occur.
    void updateLeaderBeatDistance(double beatDistance) override;
    /// Enforces the immediate change of the beat distance
    void forceUpdateLeaderBeatDistance(double beatDistance);
    /// Must never result in a call to SyncableListener::notifyBpmChanged or
    /// signal loops could occur.
    void updateLeaderBpm(mixxx::Bpm bpm) override;
    void notifyLeaderParamSource() override;
    /// Combines the above three calls into one, since they are often set
    /// simultaneously.  Avoids redundant recalculation that would occur by
    /// using the three calls separately.
    void reinitLeaderParams(double beatDistance, mixxx::Bpm baseBpm, mixxx::Bpm bpm) override;
    /// Must never result in a call to
    /// SyncableListener::notifyInstantaneousBpmChanged or signal loops could
    /// occur.
    void updateInstantaneousBpm(mixxx::Bpm bpm) override;
    void onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac);
    void onCallbackEnd(int sampleRate, size_t bufferSize);   

    void setMidiClockOutController(Controller* pMidiClockOutController); ///< Sets the controller mapped to "Midi Clock Out"; triggered when a signal is emitted to EngineMixer from the ControllerManager
    void deleteMidiClockOutController(); ///< Cleans up to avoid dangling pointers; triggered from ControllerManager during shutdown

  signals:
    void clockTick(double value, QObject* pSender);
    void clockStart(double value, QObject* pSender);
    void clockContinue(double value, QObject* pSender);
    void clockStop(double value, QObject* pSender);
  
  private slots:     
    void slotDummy();
    void tick();    

    void slotControlOutEnabled(double controlButtonValue);
    void slotControlRestart(double controlButtonValue);
    void slotControlTick(double controlButtonValue);
    void slotControlNudgeFwd(double controlButtonValue);
    void slotControlNudgeBack(double controlButtonValue);

  private:
    // ableton::link::HostTimeFilter<MixxxClockRef> m_hostTimeFilter;
    QString m_group; ///< String for MidiClockOut in debug and controller, control object access
    EngineSync* m_pEngineSync; ///< Unowned, must outlive this class
    SyncMode m_syncMode; ///< Syncables mode; either Follower or None or Invalid    

    mixxx::Bpm m_currentBpm; ///< Tempo equivalent to mV_currentTickLength      

    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;    
    std::chrono::steady_clock::time_point m_plannedNextTickTime; ///< Next planned tick

    std::chrono::steady_clock::time_point mV_adjustedTimeReceivedNewBpm; ///< For calculating next timestamp with the new interval; multi-threaded    
    std::chrono::nanoseconds mV_tempoChangeSyncAdjustment; ///< Cumulative sync adjustments as tempo change happens between ticks; multi-threaded        
    
    std::chrono::microseconds mV_currentTickLength; ///< Time between ticks; multithreaded
    std::chrono::microseconds m_newTickLength; ///< Stores the latest tick interval value not-yet incorporated into the tick timer interval
    std::chrono::nanoseconds m_intervalLength; // Current tick timer interval. TODO(Tuuli) Does this need to be stored, removed?
    
    double m_beatDistance; ///< The beat position of the leader when received
    std::chrono::steady_clock::time_point m_timeReceivedBeatDistance; ///< The time when the leaders beatDistance was received
    uint32_t m_ticksReceivedBeatDistance; ///< The ticks when the leaders beatDistance was received

    bool m_enabled; ///< Enable or disable timing MidiClockOut ticks

    /// 24PPQN ticks     
    uint32_t m_tickCount; ///< Number of ticks (24 PPQN)
    uint8_t m_sixteenths; ///< Number of sixteenth notes (4 PPQN)
    uint8_t m_beats; ///< Number of beats (1 PPQN)
    uint32_t m_bars; ///< Number of bars; 4 beats per bar
    // TODO(Tuuli): add a setting to change the meter from 4/4
    
    bool mflag_bpmChangedThisBar; ///< Used to report bar-length accuracy for steady-BPM bars
  
    uint32_t mV_ticksSinceBpmChange; ///< Counter to use with mV_adjustedTimeReceivedNewBpm to calculate timepoints; multithreaded
    
    int16_t mV_tickAdjustment; ///< Number of ticks to skip or spam to beatjump or otherwise adjust position on external sequencers; multithreaded. Positive numbers mean the MidiClockOut ticks are behind the SyncLeaders phase and need to catchup.

    #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QChronoTimer m_ticknsTimer = QChronoTimer(nullptr);
    #else
    QTimer m_ticknsTimer = QTimer(nullptr); 
    #endif
    Qt::TimerId m_ticknsTimerID;

    //Tempo
    void handleNewBPM(mixxx::Bpm newBpm); 
    std::chrono::microseconds tickLengthFromBpm(double bpm); ///< 24 ppqn tick length in microseconds; mixxx::bpm supports 0 to 500 tempo range
    
    //Sync
    void restart(); ///< Restart all tick counters, all bpm adjusters, and the tick clock (if its running)
    void backSixteenth(); ///< Move external device back 6 ticks
    void fwdSixteenth(); ///< Move external device forward 6 ticks

    void forceGetBeatDistance(); // Is this possible?   
    void adjustSyncTicks(int16_t tickAdjustment); ///< Plans a tick adjustment; thread reads and sends extra ticks, or skips ticks
    void resetQueuedSyncTicks();                  ///< Resets planned extra or skipped ticks to zero.

    //MIDI
    bool sendDirectRTMidi(uint8_t status); ///< Sends 3 byte {status, 00, 00} to m_pMidiClockOutController

    void sendMidiClockTick(); ///< Sends 0xF8 to portMidi device with midi_clock_out script mapped
    void sendMidiClockStart(); ///< Sends 0xFA to portMidi device with midi_clock_out script mapped
    void sendMidiClockContinue(); ///< Sends 0xFB to portMidi device with midi_clock_out script mapped
    void sendMidiClockStop(); ///< Sends 0xFC to portMidi device with midi_clock_out script mapped

    Controller* m_pMidiClockOutController = nullptr; ///< Duplicate unowned pointer to the Controller that is linked to Midi Clock Out mapping       

    std::unique_ptr<MidiClockOutThread> m_pMidiClockOutThread; ///< Thread for timing and Midi out


    //Debug 
    std::chrono::microseconds m_barLengthMeasured;
    std::chrono::microseconds m_barLengthError;
    std::chrono::steady_clock::time_point m_timeReceivedNewBpm;
    std::chrono::steady_clock::time_point m_endTime;
    uint32_t m_debugTickCounter;
    bool m_timingStyleThread;
    bool m_midiStyleThread;
    void debugBarTime();

    //Control objects
    std::unique_ptr<ControlPushButton> m_pMidiClockEnableButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockRestartButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockTickButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeFwdButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeBackButton;
    std::unique_ptr<ControlObject> m_pMidiClockPosSixteenths;
    std::unique_ptr<ControlObject> m_pMidiClockPosBeats;
    std::unique_ptr<ControlObject> m_pMidiClockPosBars;

    std::unique_ptr<ControlObject> m_pMidiClockTick;
    std::unique_ptr<ControlObject> m_pMidiClockStart;
    std::unique_ptr<ControlObject> m_pMidiClockContinue;
    std::unique_ptr<ControlObject> m_pMidiClockStop;

    std::chrono::microseconds getHostTime() const;
    std::chrono::microseconds getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const;    

    friend class MidiClockOutThread;
    };
