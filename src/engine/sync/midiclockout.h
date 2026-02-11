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

//or std::chrono::microseconds?
//using MixxxClockRef = std::chrono::steady_clock; 
using MixxxClockRef = ableton::platforms::stl::Clock;

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

    void testMessage();

    
    void backSixteenth();
    void fwdSixteenth();
  
  private slots:    
    void tick();
    //void debugTestAllTheTimers(double controlButtonValue);

    void slotControlOutEnabled(double controlButtonValue);
    void slotControlRestart(double controlButtonValue);
    void slotControlTick(double controlButtonValue);
    void slotControlNudgeFwd(double controlButtonValue);
    void slotControlNudgeBack(double controlButtonValue);

  private:
    // ableton::link::HostTimeFilter<MixxxClockRef> m_hostTimeFilter;

    //QChronoTimerType m_ticknsTimer = QChronoTimerType(nullptr);
    //QChronoTimerType m_debugTimer = QChronoTimerType(nullptr);

    #if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)    
    QChronoTimer m_ticknsTimer = QChronoTimer(nullptr);
    QChronoTimer m_debugTimer = QChronoTimer(nullptr);   
    #else
    QTimer m_ticknsTimer = QTimer(nullptr);
    QTimer m_debugTimer = QTimer(nullptr);
    #endif

    Qt::TimerId m_ticknsTimerID;

    QString m_group;
    EngineSync* m_pEngineSync; // unowned, must outlive this.
    SyncMode m_syncMode;

    mixxx::Bpm m_oldTempo;
    mixxx::Bpm m_currentBpm;
    mixxx::Bpm m_newBpm;
    double m_dnewBpm;


    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;
    std::chrono::microseconds m_nextTickTime;
    std::chrono::microseconds m_plannedNextTickTime;    
    std::chrono::microseconds m_newNextTickTime;
    std::chrono::microseconds m_differenceTickLength;

    std::chrono::microseconds m_timeReceivedNewLeaderBpm;    
    std::chrono::microseconds m_timeReceivedNewLeaderBpmLate;    
    std::chrono::microseconds m_maximumNextTickCutoffTime;
    

    std::chrono::microseconds tickLengthFromBpm(double bpm);
    std::chrono::microseconds m_currentTickLength;
    std::chrono::microseconds m_newTickLength;
    std::chrono::microseconds m_tickCutOff;    
    std::chrono::nanoseconds m_intervalLength;

    mixxx::audio::FramePos m_beatDistance;

    bool mflag_plannedTickWillBeLate;
    bool mflag_useNewInsteadOfPlannedTickTime;
    bool mflag_bpmChangedThisBar;

    bool m_enabled;   

    /// 24PPQN ticks     
    uint32_t m_tickCount;
    uint8_t m_sixteenths;
    uint8_t m_beats;
    uint32_t m_bars;

    int32_t m_tickError;
    uint32_t m_ticksSinceBpmChange; ///< Counter to use with m_timeReceivedNewLeaderBpm to calculate timepoints

    bool m_skipNextTick;    

    void skipTick();

    //Restart all tick counters, all bpm adjusters, and the tick clock (if its running)
    void restart();

    void sendMidiClockTick(); ///< Sends 0xF8 to portMidi device

    //Control objects
    std::unique_ptr<ControlPushButton> m_pMidiClockEnableButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockRestartButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockTickButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeFwdButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeBackButton;
    std::unique_ptr<ControlObject> m_pMidiClockPosSixteenths;
    std::unique_ptr<ControlObject> m_pMidiClockPosBeats;
    std::unique_ptr<ControlObject> m_pMidiClockPosBars;

    std::chrono::microseconds getHostTime() const;
    std::chrono::microseconds getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const;

    // Test/Debug code

    std::chrono::microseconds m_barLengthMeasured;
    std::chrono::microseconds m_barLengthError;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_endTime;

    uint32_t m_debugTickCounter;

    void debugBarTime();
    
    };
