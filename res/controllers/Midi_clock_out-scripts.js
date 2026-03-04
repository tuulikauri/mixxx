//var midi_clock_out = {}; //components framework

function midi_clock_out() { }

/// Midi Clock Out
/// Controller script to send 0xF8, 0xFA, 0xFB, and 0xFC commands in time with tempo leader
/// Connect this script to a MIDI Port to use as a Clock signal to drive external sequencers, DAWs, etc.

////////////////////////////////////////////////////////////////
///                       USER OPTIONS                       ///
////////////////////////////////////////////////////////////////

// Currently none; populate with settings to import from the GUI Settings dialog.
// var var_name = engine.getSetting("setting_text_name"); 


///////////////////////////////////////////////////////////////
//                         FUNCTIONS                         //
///////////////////////////////////////////////////////////////

midi_clock_out.init = function (id) { // called when the MIDI device is opened & set up
    midi_clock_out.id = id; // store the ID of this device for later use
    midi_clock_out.directory_mode = false;
    midi_clock_out.deck_current = -1;
    midi_clock_out.decks = [
        { id: 0, priority: 0.0, playing: false },
        { id: 1, priority: 0.0, playing: false },
        { id: 2, priority: 0.0, playing: false },
        { id: 3, priority: 0.0, playing: false }
    ];
    
    //engine.makeConnection("[MidiClockOut]", "clock_tick", midi_clock_out.outputF8); // warning [Controller] ControlDoublePrivate::getControl returning NULL for ( "[MidiClockOut]" , "clock_tick" )
    //warning[Controller] "script tried to connect to ControlObject ([MidiClockOut], clock_tick) which is non-existent."

    //engine.makeConnection("[MidiClockOut]", "clock_start", midi_clock_out.outputFA);
    //engine.makeConnection("[MidiClockOut]", "clock_continue", midi_clock_out.outputFB);
    //engine.makeConnection("[MidiClockOut]", "clock_stop", midi_clock_out.outputFC);    
};

midi_clock_out.shutdown = function (id) { // called when the MIDI device is closed
    // TODO(Tuuli): set MidiClockOut.m_enable = false // "[MidiClockOut]","out_enabled" event
};


midi_clock_out.outputF8 = function (_value, _group, _control) { // send midi note for clock tick  
    midi.sendShortMsg(0xF8, 0x00, 0x00);
};
midi_clock_out.outputFA = function (_value, _group, _control) { // send midi note for clock start  
    midi.sendShortMsg(0xFA, 0x00, 0x00);
};
midi_clock_out.outputFB = function (_value, _group, _control) { // send midi note for clock continue  
    midi.sendShortMsg(0xFB, 0x00, 0x00);
};
midi_clock_out.outputFC = function (_value, _group, _control) { // send midi note for clock stop  
    midi.sendShortMsg(0xFC, 0x00, 0x00);
};