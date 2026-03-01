#include "engine/sync/midiclockoutthread.h"
#include "engine/sync/midiclockout.h"

#include <QtDebug>

#include "moc_midiclockoutthread.cpp"
#include "preferences/usersettings.h"
#include "util/logger.h"
#include "util/runtimeloggingcategory.h"
#include "util/string.h"
#include "util/time.h"
#include "util/trace.h"
#include "util/fifo.h"

namespace {
const mixxx::Logger kLogger("MidiClockOutThread");
constexpr std::chrono::microseconds kStartTickLength{(int)(2500000.0 / 120.0)};
}

MidiClockOutThread::MidiClockOutThread(MidiClockOut* parent) : 
          QThread(parent),
          m_pMidiClockOutParent(parent),
          stopplz(false),
          m_midiFIFOQueue(512) {    
    this->setObjectName("MidiClockOutThread");    
    m_pMidiFIFOQueue = &m_midiFIFOQueue;
    qDebug() << "MidiClockOutThread::Create ";
}
MidiClockOutThread::~MidiClockOutThread() {
    qDebug() << "MidiClockOutThread::destroy ";
    mutex.lock();
    stopplz = true;
    cond.wakeOne();
    mutex.unlock();
    qDebug() << "MidiClockOutThread::waiting ";
    wait();   
    qDebug() << "MidiClockOutThread::done ";
}

// Adds status to the queue of MIDI data to send
void MidiClockOutThread::queueDirectRTMidi(uint8_t status) {
    const QMutexLocker locker(&midiMutex);
    if (m_pMidiFIFOQueue->write(&status, 1) == 1) {
        qDebug() << "MidiClockOutThread::queueDirectRTMidi" << status;
    }
}
// Sends MIDI data
bool MidiClockOutThread::sendDirectRTMidi(uint8_t status) {
    //return (m_pMidiClockOutController->sendMidi(status));
    /*
    if (m_pMidiClockOutController) {
        if (m_pMidiClockOutController->isOpen()) {
            return (m_pMidiClockOutController->sendBytes(tickMessage));
        }
    }
    */
    qDebug() << "MidiClockOutThread::sendDirectRTMidi (not sent yet; just this msg): " << status;
    return true;
}
void MidiClockOutThread::setMidiClockOutController(Controller* pMidiClockOutController) {
    m_pMidiClockOutController = pMidiClockOutController; ///< Duplicate unowned pointer to the Controller that is linked to Midi Clock Out mapping
}
void MidiClockOutThread::deleteMidiClockOutController() {
    m_pMidiClockOutController = nullptr;
}

void MidiClockOutThread::startMidiClockOutThread() {
    QMutexLocker locker(&mutex);
    if (!isRunning()) {
        // m_startTime = std::chrono::steady_clock::now();
        start(QThread::HighestPriority);
    } else
        cond.wakeOne();
    qDebug() << "MidiClockOutThread::startMidiClockOutThread";
}
void MidiClockOutThread::stopPlease() {
    mutex.lock();
    stopplz = true;
    mutex.unlock();
    wait();
}

void MidiClockOutThread::run() {
    bool stopNow = false;
    while (!stopNow) {
        midiMutex.lock();
        if (m_pMidiFIFOQueue->readAvailable()) { // DEBUG stops here instead of quitting?
            uint8_t data;
            if(m_pMidiFIFOQueue->read(&data, 1) == 1){
                midiMutex.unlock();
                sendDirectRTMidi(data);
            } else {
                midiMutex.unlock();
            }
        } else {
            midiMutex.unlock();
        }

        usleep(200);

        mutex.lock();
        stopNow = stopplz;
        mutex.unlock();
    }
}


/// OLDDDDDDDDDDDDDDD ////////////////////////////////////////////////
/// 
/// 

/*
namespace {
const mixxx::Logger kLogger("MidiClockOutThread");
constexpr std::chrono::microseconds kStartTickLength{(int)(2500000.0 / 120.0)};
const uint8_t sendStop = 1 << 1, sendContinue = 1 << 2, sendStart = 1 << 3, sendTickSyncExtra = 1 << 4, sendTickSyncSkip = 1 << 5, sendTick = 1 << 6;
} // namespace

MidiClockOutThread::MidiClockOutThread(MidiClockOut* parent)
        : QThread(parent),
          m_pMidiClockOutParent(parent),
          stopplz(false),
          mV_sendTicksEnabled(false),
          mV_tickAdjustment(0),
          mV_ticksSinceBpmChange(0),
          mV_tickFlag(false),
          mV_startFlag(false),
          mV_continueFlag(false),
          mV_stopFlag(false) {
    // uint32_t mV_ticksSinceBpmChange;
    // int16_t mV_tickAdjustment;
    // std::chrono::microseconds mV_timeReceivedNewLeaderBpm;
    // std::chrono::microseconds mV_currentTickLength;
    this->setObjectName("MidiClockOutThread");
    setTickTimerParameters(std::chrono::steady_clock::now(), kStartTickLength);
    qDebug() << "MidiClockOutThread::Create ";
}
MidiClockOutThread::~MidiClockOutThread() {
    qDebug() << "MidiClockOutThread::destroy ";
    mutex.lock();
    stopplz = true;
    cond.wakeOne();
    mutex.unlock();
    wait();
}


 

void MidiClockOutThread::startMidiClockOutThread() {
    QMutexLocker locker(&mutex);
    if (!isRunning()) {
        // m_startTime = std::chrono::steady_clock::now();
        start();
    } else
        cond.wakeOne();
    qDebug() << "MidiClockOutThread::startMidiClockOutThread";
}
void MidiClockOutThread::stopPlease() {
    mutex.lock();
    stopplz = true;
    mutex.unlock();    
}

void MidiClockOutThread::setTickTimerParameters(std::chrono::steady_clock::time_point timeTempoSet, std::chrono::microseconds tempoInterval) {
    QMutexLocker locker(&mutex);
    mV_timeReceivedNewLeaderBpm = timeTempoSet;
    mV_currentTickLength = tempoInterval;
    mV_plannedNextTickTime = mV_timeReceivedNewLeaderBpm + mV_currentTickLength;
    qDebug() << "MidiClockOutThread::setTickTimerParameters " << mV_currentTickLength;
}
void MidiClockOutThread::setTickAdjustment(int16_t adjustment) {
    QMutexLocker locker(&mutex);
    mV_tickAdjustment += adjustment;
    qDebug() << "MidiClockOutThread::setTickAdjustment ";
}
void MidiClockOutThread::resetTickAdjustment() {
    QMutexLocker locker(&mutex);
    mV_tickAdjustment = 0;
    qDebug() << "MidiClockOutThread::resetTickAdjustment ";
}


/// @brief Sets the MidiClockOut interval between 0xF8 ticks for sending clock tempo; called from main thread
/// @param interval the clock tick interval in nanoseconds
void MidiClockOutThread::setTimerInterval(std::chrono::nanoseconds interval) {
    QMutexLocker locker(&mutex);
    //m_interval = interval;
    mV_currentTickLength = std::chrono::duration_cast<std::chrono::microseconds>(interval);    
    qDebug() << "MidiClockOutThread::setTimerInterval ";
}

void MidiClockOutThread::startTicks() {
    QMutexLocker locker(&mutex);
    mV_sendTicksEnabled = true;
    qDebug() << "MidiClockOutThread::startTicks (queued) ";
}

void MidiClockOutThread::stopTicks() {
    QMutexLocker locker(&mutex);
    mV_sendTicksEnabled = false;
    qDebug() << "MidiClockOutThread::stopTicks (queued) ";
}

void MidiClockOutThread::testTickLength() {
    auto startTime = std::chrono::steady_clock::now();
    uint32_t ticks = 0;
    bool stopNow = false;
    while (!stopNow && ticks < 5000) {
        // timeNow.time_since_epoch() << ", C" << m_tickCounter;
        

        //mutex.lock();
        //if (timeNow > (m_startTime + std::chrono::seconds(10))) {
        //    stopplz = true;
        //    //TODO(Tuuli): Instead run it 1000 times, and time it.
        //}
        //mutex.unlock();
        

        ticks++;
        // int timecount = timeNow.time_since_epoch().count();

        usleep(200);

        // TODO(Tuuli): is stopplz accessed from ~MidiClockOutThread? hidiothread uses m_runLoopSemaphore
        mutex.lock();
        stopNow = stopplz;        
        mutex.unlock();

        
        //mutex.lock();
        //emit tickSent();
        ////cond.wait(&mutex);
        //mutex.unlock();
        
    }
    auto endTime = std::chrono::steady_clock::now();
    qDebug() << "MidiClockOutThread::testTickLength (10s) " << std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
}


void MidiClockOutThread::sendMidiClockTick() {    
    QMutexLocker locker(&mutex);
    mV_tickFlag = true;
    qDebug() << "MidiClockOutThread::sendMidiClockTick queued ";
}                                                 
void MidiClockOutThread::sendMidiClockStart() {
    QMutexLocker locker(&mutex);
    mV_startFlag = true;
    qDebug() << "MidiClockOutThread::sendMidiClockStart queued ";
}                                                 
void MidiClockOutThread::sendMidiClockContinue() {
    QMutexLocker locker(&mutex);
    mV_continueFlag = true;
    qDebug() << "MidiClockOutThread::sendMidiClockCont queued ";
}                                             
void MidiClockOutThread::sendMidiClockStop() {
    QMutexLocker locker(&mutex);
    mV_stopFlag = true;
    qDebug() << "MidiClockOutThread::sendMidiClockStop queued ";
}

void MidiClockOutThread::sendMidiClockTTick() {
    m_pMidiClockOutParent->sendMidiClockTick();
}
void MidiClockOutThread::sendMidiClockTStart() {
    m_pMidiClockOutParent->sendMidiClockStart();
}
void MidiClockOutThread::sendMidiClockTContinue() {
    m_pMidiClockOutParent->sendMidiClockContinue();
}
void MidiClockOutThread::sendMidiClockTStop() {
    m_pMidiClockOutParent->sendMidiClockStop();
}

void MidiClockOutThread::run() {           
    //bool stopNow = false;
    //uint8_t outputData = 0; //TODO(Tuuli): This var isnt needed, everything is handled in the if-else including the results.

    //testTickLength();

    //TODO(Tuuli): Do these need to be set? Shouldnt.. they are set when new tempo events are received, and when the thread is initialized
    //mV_ticksSinceBpmChange = 0;
    //mV_timeReceivedNewLeaderBpm = std::chrono::steady_clock::now();    
    //mV_currentTickLength = kStartTickLength;   

    while (!stopplz) {
        // Output in priority order: stop, continue, start, tick                      

        // Check for start, stop commands
        mutex.lock();
        if (mV_stopFlag) {
            //outputData = sendStop;
            mV_stopFlag = false;
            //mV_sendTicksEnabled = false; // TODO(Tuuli): does it make sense to stop ticking when we stop?
            mutex.unlock();
            qDebug() << "MidiClockOutThread::run STOP ";
            sendMidiClockTStop();
        } 
        else if (mV_continueFlag) {
            //outputData = sendContinue;
            mV_continueFlag = false;
            mutex.unlock();
            qDebug() << "MidiClockOutThread::run CONTINUE ";
            sendMidiClockTContinue();
        } 
        else if (mV_startFlag) {
            //outputData = sendStart;
            mV_startFlag = false;
            //mV_sendTicksEnabled = true; //TODO(Tuuli): does it make sense to start ticking when we start?
            mutex.unlock();
            qDebug() << "MidiClockOutThread::run START ";
            sendMidiClockTStart();
        } 
        // Check for sync shifts
        else if (mV_tickAdjustment > 0) {
            //outputData = sendTickSyncExtra;
            mV_tickAdjustment -= 1;
            mutex.unlock();
            qDebug() << "MidiClockOutThread::run SYNCTICK ";            
            sendMidiClockTTick();            
        }         
        // Check for tempo ticks
        else if (mV_sendTicksEnabled) {            
            if (std::chrono::steady_clock::now() > mV_plannedNextTickTime) {
                if (mV_tickAdjustment < 0) {
                    //outputData = sendTickSyncSkip;
                    mV_tickAdjustment++;
                    mutex.unlock();
                    qDebug() << "MidiClockOutThread::run SKIPTICK ";
                } else {
                    mutex.unlock();
                    //outputData = sendTick;
                    qDebug() << "MidiClockOutThread::run TEMPOTICK ";
                    sendMidiClockTTick();  
                    emit tickSent();
                }
                mutex.lock();
                mV_ticksSinceBpmChange++;
                mV_plannedNextTickTime = mV_timeReceivedNewLeaderBpm + mV_currentTickLength * mV_ticksSinceBpmChange;                       
            }            
            mutex.unlock();
        } 
        else {
            mutex.unlock();
        }

        usleep(200);

        
        //mutex.lock();
        //stopNow = stopplz;
        //mutex.unlock();
        
    }
    mutex.lock();    
    cond.wait(&mutex);
    mutex.unlock();
}
*/
