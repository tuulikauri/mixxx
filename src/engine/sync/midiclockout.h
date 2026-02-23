#pragma once
#include <ableton/platforms/stl/Clock.hpp>

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

/// This class manages a Midi clock output (0xF8)

/// This class manages a Midi clock output (0xF8). It prioritizes maintaining the
/// user-selected beat_distance, so the external sequencers are aligned to the beat
/// grid in Mixxx.
/// Object is initialized in EngineSync constructor
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

    void backSixteenth();
    void fwdSixteenth();

  signals:
    void clockTick(double value, QObject* pSender);
    void clockStart(double value, QObject* pSender);
    void clockContinue(double value, QObject* pSender);
    void clockStop(double value, QObject* pSender);

  private slots:    
    void callTick();
    void tick(uint8_t recurse_count);
    //void debugTestAllTheTimers(double controlButtonValue);

    void slotControlOutEnabled(double controlButtonValue);
    void slotControlRestart(double controlButtonValue);
    void slotControlTick(double controlButtonValue);
    void slotControlNudgeFwd(double controlButtonValue);
    void slotControlNudgeBack(double controlButtonValue);

  private:
    // ableton::link::HostTimeFilter<MixxxClockRef> m_hostTimeFilter;
    QString m_group; ///< String for MidiClockOut in debug and controller, control object access
    EngineSync* m_pEngineSync; ///< Unowned, must outlive this class (copied from AbletonLink)
    SyncMode m_syncMode; ///< Syncables mode; either Follower or None or Invalid

    mixxx::Bpm m_currentBpm; ///< Tempo equivalent to mV_currentTickLength
    mixxx::Bpm m_oldTempo;
    mixxx::Bpm m_currentBpm;
    mixxx::Bpm m_newBpm;

    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;
    std::chrono::steady_clock::time_point m_plannedNextTickTime; //?
    std::chrono::microseconds m_newNextTickTime; //?
    std::chrono::microseconds m_differenceTickLength; //?

    std::chrono::steady_clock::time_point mV_adjustedTimeReceivedNewBpm; ///< For calculating next timestamp with the new interval; multi-threaded    
    std::chrono::microseconds m_timeReceivedNewLeaderBpmLate; //?    
    std::chrono::microseconds m_maximumNextTickCutoffTime; //?
    

    std::chrono::microseconds tickLengthFromBpm(double bpm); ///< 24 ppqn tick length in microseconds; mixxx::bpm supports 0 to 500 tempo range
    std::chrono::microseconds mV_currentTickLength; ///< Time between ticks; multithreaded
    std::chrono::microseconds m_newTickLength;
    std::chrono::microseconds m_tickCutOff;    
    std::chrono::nanoseconds m_intervalLength;

    mixxx::audio::FramePos m_newBeatDistance;
    mixxx::audio::FramePos m_beatDistance; ///< The beat position of MidiClockOut clock

    bool m_enabled; ///< Enable or disable outputting MidiClockOut ticks

    /// 24PPQN ticks     
    uint32_t m_tickCount; ///< Number of ticks (24 PPQN)
    uint8_t m_sixteenths; ///< Number of sixteenth notes (4 PPQN)
    uint8_t m_beats; ///< Number of beats (1 PPQN)
    uint32_t m_bars; ///< Number of bars; 4 beats per bar
    // TODO(Tuuli): add a setting to change the meter from 4/4

    bool mflag_plannedTickWillBeLate; //?
    bool mflag_useNewInsteadOfPlannedTickTime; //?
    bool mflag_bpmChangedThisBar; ///< Used to report bar-length accuracy for steady-BPM bars

    int32_t m_tickSyncOffset; ///< Stores sync tick offsets; difference from the latest update of the leaders sync position to MidiClockOuts sync position. Positive numbers mean the MidiClockOut ticks are behind the SyncLeaders phase and need to catchup.
    uint32_t mV_ticksSinceBpmChange; ///< Counter to use with mV_adjustedTimeReceivedNewBpm to calculate timepoints; multithreaded

    bool m_skipNextTick;    
    int16_t mV_tickAdjustment; ///< Number of ticks to skip or spam to beatjump or otherwise adjust position on external sequencers; multithreaded   

        // QChronoTimerType m_ticknsTimer = QChronoTimerType(nullptr);
    // QChronoTimerType m_debugTimer = QChronoTimerType(nullptr);

    #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QChronoTimer m_ticknsTimer = QChronoTimer(nullptr);
    QChronoTimer m_debugTimer = QChronoTimer(nullptr);
    #else
    QTimer m_ticknsTimer = QTimer(nullptr);
    QTimer m_debugTimer = QTimer(nullptr);
    #endif

    Qt::TimerId m_ticknsTimerID;

    void handleTickSyncOffset();
    void handleNewBPM(); 
    void skipTick();

    void forceGetBeatDistance();

    //Restart all tick counters, all bpm adjusters, and the tick clock (if its running)
    void restart();

    void sendMidiClockTick(); ///< Sends 0xF8 to portMidi device with midi_clock_out script mapped
    void adjustSyncTicks(int16_t tickAdjustment); ///< Plans a tick adjustment; thread reads and sends extra ticks, or skips ticks
    void resetQueuedSyncTicks(); ///< Resets planned extra or skipped ticks to zero.
    void sendMidiClockStart(); ///< Sends 0xFA to portMidi device with midi_clock_out script mapped
    void sendMidiClockContinue(); ///< Sends 0xFB to portMidi device with midi_clock_out script mapped
    void sendMidiClockStop(); ///< Sends 0xFC to portMidi device with midi_clock_out script mapped

    //Debug 
    std::chrono::microseconds m_barLengthMeasured;
    std::chrono::microseconds m_barLengthError;
    std::chrono::steady_clock::time_point m_timeReceivedNewBpm;
    std::chrono::steady_clock::time_point m_endTime;
    uint32_t m_debugTickCounter;
    bool m_timingStyleThread;
    bool m_midiStyleThread;

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
    void debugBarTime();
    
    };
