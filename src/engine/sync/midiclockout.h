/// @file midiclockout.h
/// @brief This class manages a Midi clock output (0xF8, 0xFA, 0xFB, 0xFC).
#pragma once
#include <ableton/platforms/stl/Clock.hpp>
#include <chrono>

#include "control/controlpushbutton.h"
#include "controllers/controller.h"
#include "engine/channels/enginechannel.h"
#include "engine/enginebuffer.h"
#include "engine/sync/midiclockoutthread.h"
#include "engine/sync/syncable.h"
#include "engine/sync/synccontrol.h"

/// @brief This class manages a Midi clock output (0xF8, 0xFA, 0xFB, 0xFC).
/// @callergraph
/// @callgraph
/// This prioritizes maintaining the
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
/// splines=polyline;
/// label = "MidiClockOut Logic";
/// tooltop = "Error-correction logic for MidiClockOut";
/// node[fontsize=10 shape=Mrecord];
/// edge[fontsize=10];
/// LISTENING [tooltip = "not playing, but tracking sync and tempo"];
/// RUNNING [tooltip = "clock is running, 0xF8 ticks"];
///
/// LISTENING->RUNNING[label = "enable"];
/// RUNNING->ONBEAT[label = "enable"];
/// RUNNING->LISTENING[label = "disable"];
/// subgraph running_graph {
/// ONBEAT [tooltip = "clock is running onbeat"];
/// OFFBEAT [tooltip = "clock is undesirably offbeat"];
///
/// ONBEAT->OFFBEAT[label = "drift" labeltooltip = "clock drift from non-realtime OS performance"];
/// ONBEAT->OFFBEAT[label = "tempo" labeltooltip = "mid-tick tempo change, or short instantaneous tempo change"];
/// ONBEAT->OFFBEAT[label = "beatjump" labeltooltip = "beatjump" ];
/// ONBEAT->OFFBEAT[label = "scratch" labeltooltip = "scratching" ];
///
/// OFFBEAT->ONBEAT[label = "drift fix" labeltooltip = "automatic clock drift correction"];
/// OFFBEAT->ONBEAT[label = "beatjump fix" labeltooltip = "automatic clock beatjump correction"];
/// OFFBEAT->PENDINGFIX[label = "auto-request" labeltooltip = "clock requests beatDistance"];
/// PENDINGFIX->ONBEAT[label = "leader" labeltooltip = "leader sends beatDistance"];
/// OFFBEAT->ONBEAT[label = "restart" labeltooltip = "DJ restarts"];
/// OFFBEAT->ONBEAT[label = "nudge" labeltooltip = "DJ nudges phase"];
/// }
/// }
/// @enddot

// TODO(Tuuli): is something like this needed?
// using MixxxClockRef = ableton::platforms::stl::Clock;

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
    /// The following functions are used to tell syncables about the state of the current Sync Master.
    
    /// Must never result in a call to SyncableListener::notifyBeatDistanceChanged or signal loops could occur.
    void updateLeaderBeatDistance(double beatDistance) override;
    /// Enforces the immediate change of the beat distance
    void forceUpdateLeaderBeatDistance(double beatDistance);
    /// @brief Update from [EngineSync]
    /// @details Also called from reinitLeaderParams updateInstantaneousBpm. Must never result in a call to SyncableListener::notifyBpmChanged or signal loops could occur.
    void updateLeaderBpm(mixxx::Bpm bpm) override;
    void notifyLeaderParamSource() override;
    /// Combines the above three calls into one, since they are often set
    /// simultaneously. Avoids redundant recalculation that would occur by
    /// using the three calls separately.
    void reinitLeaderParams(double beatDistance, mixxx::Bpm baseBpm, mixxx::Bpm bpm) override;
    /// Must never result in a call to SyncableListener::notifyInstantaneousBpmChanged 
    /// or signal loops could occur.
    void updateInstantaneousBpm(mixxx::Bpm bpm) override;
    void onCallbackStart(std::chrono::microseconds absTimeWhenPrevOutputBufferReachesDac);
    void onCallbackEnd(int sampleRate, size_t bufferSize);

    void setMidiClockOutController(Controller* pMidiClockOutController); ///< Sets the controller mapped to "Midi Clock Out"; triggered when a signal is emitted to EngineMixer from the ControllerManager
    void deleteMidiClockOutController();                                 ///< Cleans up to avoid dangling pointers; triggered from ControllerManager during shutdown

  private slots:
    void tickGui(int32_t syncTicks); ///< Advances the bars:beats:sixteenths syncTicks+1 times forward in the GUI. Should be used to match the beat position of an external sequencer. Should not tick for skipped or failed ticks, only when a tick is sent to external sequencers.

    void slotControlOutEnabled(double controlButtonValue);
    void slotControlRestart(double controlButtonValue);
    void slotControlTest(double controlButtonValue);
    void slotControlNudgeFwd(double controlButtonValue);
    void slotControlNudgeBack(double controlButtonValue);

  private:
    // ableton::link::HostTimeFilter<MixxxClockRef> m_hostTimeFilter;
    QString m_group;           ///< String for MidiClockOut in debug and controller, control object access
    EngineSync* m_pEngineSync; ///< Unowned, must outlive this class
    SyncMode m_syncMode;       ///< Syncables mode; either Follower or None or Invalid

    mixxx::Bpm m_currentBpm; ///< Latest BPM set by the Leader; mixxx::bpm supports 0 to 500 tempo range

    std::chrono::microseconds m_absTimeWhenPrevOutputBufferReachesDac;

    bool m_enabled; ///< Enable or disable timing MidiClockOut ticks
    bool m_uniquePlaying; ///< Track if it's the only playing syncable; follows sync jumps from updateLeaderBeatDistance is this is false, ie another leader is playing

    /// 24PPQN ticks
    uint32_t m_tickCount; ///< Number of ticks (24 PPQN)
    uint8_t m_sixteenths; ///< Number of sixteenth notes (4 PPQN)
    uint8_t m_beats;      ///< Number of beats (1 PPQN)
    uint32_t m_bars;      ///< Number of bars; 4 beats per bar
    // TODO(Tuuli): add a setting to change the meter from 4/4

    // Sync
    void restart();       ///< Restart all tick counters, all bpm adjusters, and the tick clock (if its running)
    void resetGui();      ///< Resets the GUI sixteenths, beats, bars to 1,1,1
    void backSixteenth(); ///< Move external device back 6 ticks
    void fwdSixteenth();  ///< Move external device forward 6 ticks

    void forceGetBeatDistance(); ///< 

    // MIDI
    bool sendDirectRTMidi(uint8_t status); ///< Sends 3 byte {status, 00, 00} to m_pMidiClockOutController

    void sendMidiClockTick();     ///< Sends 0xF8 to portMidi device with midi_clock_out script mapped
    void sendMidiClockStart();    ///< Sends 0xFA to portMidi device with midi_clock_out script mapped
    void sendMidiClockContinue(); ///< Sends 0xFB to portMidi device with midi_clock_out script mapped
    void sendMidiClockStop();     ///< Sends 0xFC to portMidi device with midi_clock_out script mapped

    Controller* m_pMidiClockOutController = nullptr; ///< Unowned pointer to the Controller that is linked to Midi Clock Out mapping
    std::unique_ptr<MidiClockOutThread> m_pMidiClockOutThread; ///< Thread for timing and Midi out

    // Control objects
    std::unique_ptr<ControlPushButton> m_pMidiClockEnableButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockRestartButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeFwdButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockNudgeBackButton;
    std::unique_ptr<ControlPushButton> m_pMidiClockTestButton;
    std::unique_ptr<ControlObject> m_pMidiClockPosSixteenths;
    std::unique_ptr<ControlObject> m_pMidiClockPosBeats;
    std::unique_ptr<ControlObject> m_pMidiClockPosBars;

    std::chrono::microseconds getHostTime() const;
    std::chrono::microseconds getHostTimeAtSpeaker(std::chrono::microseconds hostTime) const;

    friend class MidiClockOutThread;
};
