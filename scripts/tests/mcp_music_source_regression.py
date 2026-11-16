#!/usr/bin/env python3
"""Run the board's real music controller with mocked UART replies and audio IO.

Requires a host C++17 compiler. This checks delayed pause and cancellation;
it does not verify a physical 701's Bluetooth implementation.
Run from any directory: python3 scripts/tests/mcp_music_source_regression.py
"""
from pathlib import Path
import subprocess
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
template<class... Args> void discard_log(Args&&...) {}
#define ESP_LOGI(...) discard_log(__VA_ARGS__)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
static int current_task=1;
int xTaskGetCurrentTaskHandle() { return current_task; }
static int elapsed_ms=0;
static int64_t now_us=1000000;
int64_t esp_timer_get_time() { return now_us; }
static std::function<void(int)> delay_hook;
struct PollStepComplete {};
void vTaskDelay(int ms) {
    if(current_task==3) { assert(ms==250); throw PollStepComplete{}; }
    assert(current_task==1); elapsed_ms+=ms; now_us+=int64_t(ms)*1000; if(delay_hook) delay_hook(ms);
}
using TaskHandle_t=void*;
static void (*monitor_entry)(void*)=nullptr;
static void* monitor_arg=nullptr;
constexpr int pdPASS=1;
int xTaskCreate(void (*entry)(void*),const char* name,int stack,void* arg,int priority,TaskHandle_t* handle) {
    assert(std::string(name)=="701_play_status" && stack==2048 && priority==3);
    monitor_entry=entry; monitor_arg=arg; *handle=reinterpret_cast<void*>(3); return pdPASS;
}
enum DeviceState { kDeviceStateUnknown,kDeviceStateStarting,kDeviceStateWifiConfiguring,kDeviceStateActivating,kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking };
enum AbortReason { kAbortReasonNone };
enum ListeningMode { kListeningModeAutoStop,kListeningModeRealtime,kListeningModeManualStop };
enum AecMode { kAecOff };
namespace Lang { namespace Sounds { const std::string OGG_NET_DISCONNECT="offline"; } }
static std::vector<std::string> commands;
struct vbAudioCodec {
    bool blocked=false;
    void SetPlaybackBlocked(bool value) { blocked=value; }
};
static vbAudioCodec codec;
struct AudioService {
    bool voice=true,drained=true;
    int resets=0,waits=0;
    std::function<void()> during_wait;
    bool IsInitialize() { return true; }
    void EnableVoiceProcessing(bool enabled) { voice=enabled; }
    void ResetDecoder() { ++resets; }
    bool WaitForPlayCompletion(int timeout) {
        assert(timeout==200 && codec.blocked); ++waits;
        if(during_wait) { auto callback=std::move(during_wait); during_wait=nullptr; callback(); }
        return drained;
    }
    int GetMutePacket() { return 0; }
};
struct Protocol {
    bool opened=false;
    int aborts=0,stops=0,closes=0,opens=0,wakes=0;
    std::function<void()> during_open;
    bool IsAudioChannelOpened() { return opened; }
    bool OpenAudioChannel() { ++opens; opened=true; if(during_open) { auto cb=std::move(during_open); during_open=nullptr; cb(); } return true; }
    void CloseAudioChannel() { ++closes; opened=false; }
    void SendAbortSpeaking(AbortReason) { ++aborts; }
    void SendStopListening() { ++stops; }
    void SendAudio(int) {}
    void SendWakeWordDetected(const std::string&) { assert(!codec.blocked); ++wakes; }
};
struct Application {
    std::atomic<bool> chat_enabled_{true};
    std::atomic<uint32_t> chat_generation_{0};
    bool aborted_=false;
    DeviceState device_state_=kDeviceStateIdle;
    AecMode aec_mode_=kAecOff;
    AudioService audio_service_;
    std::unique_ptr<Protocol> protocol_=std::make_unique<Protocol>();
    std::deque<std::function<void()>> jobs;
    int main_event_loop_task_handle_=1;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> cb) { jobs.push_back(std::move(cb)); }
    void SetChatEnabled(bool);
    bool IsChatEnabled() { return chat_enabled_; }
    DeviceState GetDeviceState() { return device_state_; }
    AudioService& GetAudioService() { return audio_service_; }
    void SetDeviceState(DeviceState state) { if(chat_enabled_ || state==kDeviceStateIdle) device_state_=state; }
    void SetListeningMode(ListeningMode) { SetDeviceState(kDeviceStateListening); }
    void ToggleChatState();
    void WakeWordInvoke(const std::string&);
    void AbortSpeaking(AbortReason) { if(chat_enabled_) aborted_=true; }
    void PlaySound(const std::string&) { assert(!codec.blocked); }
    void Run() { assert(current_task==1); int iterations=0; while(!jobs.empty()) { assert(++iterations<1000); auto cb=std::move(jobs.front()); jobs.pop_front(); cb(); } }
};
using Callback=std::function<void(uint32_t,void*,uint16_t,void*)>;
static std::map<uint32_t,Callback> event_callbacks;
constexpr uint32_t VB_EVT_MODE_CHANGE=3,VB_EVT_STATUS_CHANGE=4,VB_EVT_MUSIC_TITLE=5,
    VB_EVT_MUSIC_LYRC=6,VB_EVT_MUSIC_TIME=7,VB_EVT_PLAY_STATUS_RESPONSE=14,VB_EVT_BT_STATUS_CHANGE=15;
int vb_event_register(uint32_t event,Callback cb,void*) { event_callbacks[event]=cb; return 0; }
void status(int value) { uint8_t data=value; int previous=current_task; current_task=2; event_callbacks.at(4)(4,&data,1,nullptr); current_task=previous; }
void vb_api_request_play_status() { commands.push_back("initial-query"); }
void vb_api_set_music_play(uint8_t value) { commands.push_back(value ? "play" : "pause"); }
void vb_api_set_music_next_prev(uint8_t value) { commands.push_back(value ? "next" : "previous"); }
constexpr uint8_t VB_MUSIC_MODE_BT=0, VB_MUSIC_MODE_TF=1, VB_MUSIC_MODE_MAX=2;
static uint8_t selected_mode=0;
static bool mode_query_fails=false, mode_switch_fails=false, status_query_fails=false;
static int query_count=0;
static uint8_t playback_query_value=0;
static std::function<void()> query_hook, mode_hook;
int vb_api_query_play_status(uint8_t* result) {
    assert(current_task==1 && codec.blocked);
    commands.push_back("query"); ++query_count;
    if(query_hook) query_hook();
    if(status_query_fails) return -1;
    *result=playback_query_value; return 0;
}
int vb_api_query_music_mode(uint8_t* result) {
    assert(current_task==1 && codec.blocked);
    commands.push_back("query-mode");
    if(mode_hook) mode_hook();
    if(mode_query_fails) return -1;
    *result=selected_mode; return 0;
}
void vb_api_set_music_mode(uint8_t mode) {
    assert(current_task==1 && codec.blocked && !Application::GetInstance().IsChatEnabled());
    assert(mode<2);
    commands.push_back(mode==0 ? "select-bt" : "select-tf");
    if(!mode_switch_fails) selected_mode=mode;
}
using ReturnValue=std::string;
constexpr int kPropertyTypeBoolean=1;
struct Property {
    bool boolean=false;
    Property(const char*,int) {}
    template<class T> T value() const { return T(boolean); }
};
struct PropertyList {
    Property prop{"play_pause",1};
    PropertyList()=default;
    PropertyList(std::initializer_list<Property>) {}
    const Property& operator[](const char*) const { return prop; }
};
struct McpServer {
    std::map<std::string,std::function<ReturnValue(const PropertyList&)>> tools;
    static McpServer& GetInstance() { static McpServer server; return server; }
    void AddTool(const std::string& name,const std::string&,const PropertyList&,std::function<ReturnValue(const PropertyList&)> cb) { tools[name]=cb; }
};
'''

MAIN = r'''
#include <algorithm>
int count(const char* name) { return std::count(commands.begin(),commands.end(),name); }
void emit_event(uint32_t event,void* data,uint16_t len) {
    int previous=current_task; current_task=2;
    event_callbacks.at(event)(event,data,len,nullptr); current_task=previous;
}
void poll_once() {
    int previous=current_task; current_task=3;
    try { monitor_entry(monitor_arg); } catch(const PollStepComplete&) {}
    current_task=previous;
}
void reset_observations() {
    commands.clear(); elapsed_ms=0; query_count=0;
    playback_query_value=0;
    query_hook=nullptr; mode_hook=nullptr; delay_hook=nullptr;
    mode_query_fails=false; mode_switch_fails=false; status_query_fails=false;
    Application::GetInstance().audio_service_.drained=true;
}
int main() {
    auto& app=Application::GetInstance(); McpMusic::Init(&codec);
    PropertyList play, pause; play.prop.boolean=true; pause.prop.boolean=false;
    auto wake=[&] { McpMusic::WakeWord("你好"); app.Run(); };
    auto ready=[&] { assert(app.IsChatEnabled() && !codec.blocked); };
    auto blocked=[&] { assert(!app.IsChatEnabled() && codec.blocked); };
    // An early zero query cannot release known Bluetooth ownership on its own.
    status(1); app.Run(); reset_observations();
    wake(); blocked();
    assert(count("pause")==1 && !count("select-tf") && query_count>=30);
    assert(elapsed_ms>=7000);
    // An already observed real pause permits a quick wake without a source switch.
    status(0); app.Run();
    reset_observations(); wake(); ready();
    assert(!count("select-bt") && !count("select-tf") && count("pause")==1 && elapsed_ms<1000);
    // Explicit previous/next/resume selects BT before sending music commands.
    for(const char* tool : {"self.bl_music.prev","self.bl_music.next","self.bl_music.set_play_or_pause"}) {
        reset_observations(); selected_mode=1;
        McpServer::GetInstance().tools.at(tool)(play); app.Run(); blocked();
        assert(selected_mode==0 && count("select-bt")==1 && count("play")==1);
        if(std::string(tool)=="self.bl_music.prev") assert(count("previous")==1);
        if(std::string(tool)=="self.bl_music.next") assert(count("next")==1);
    }
    // A real pause event enables chat without switching away from BT.
    status(1); app.Run(); reset_observations();
    query_hook=[&] { if(query_count==1) status(0); };
    wake(); ready();
    assert(selected_mode==0 && count("pause")==1 && !count("select-tf") && elapsed_ms<1000);
    // Hardware trace: pause notification arrives roughly five seconds later.
    // During the entire delay, neither TTS nor another pause/source switch is allowed.
    status(1); app.Run(); reset_observations();
    bool pause_reported=false;
    delay_hook=[&](int) {
        assert(codec.blocked && count("pause")==1 && !count("select-tf"));
        if(!pause_reported && elapsed_ms>=5000) { pause_reported=true; status(0); }
    };
    wake(); ready();
    assert(pause_reported && elapsed_ms>=5000 && query_count>=25);
    assert(count("pause")==1 && !count("select-tf"));
    // Missing/invalid playback replies must leave TTS blocked after a real pause.
    reset_observations(); status_query_fails=true;
    wake(); blocked(); assert(count("pause")==1 && elapsed_ms>=7000);
    // No TF card or source query is needed for a confirmed Bluetooth pause.
    reset_observations(); mode_query_fails=true; mode_switch_fails=true;
    status(0); app.Run(); wake(); ready(); assert(!count("query-mode"));
    // A fresh playing event overrides pause verification, even with zero queries.
    selected_mode=0; status(1); app.Run(); reset_observations();
    query_hook=[&] { if(query_count==1) status(0); if(query_count==2) status(1); };
    wake(); blocked(); assert(elapsed_ms>=7000 && count("pause")==1);
    // A later genuine pause lets the next wake recover.
    reset_observations(); status(0); app.Run(); wake(); ready();
    // Pending output must drain before any successful source handover.
    selected_mode=0; status(1); app.Run(); reset_observations(); app.audio_service_.drained=false;
    wake(); blocked();
    app.audio_service_.drained=true; reset_observations(); status(0); app.Run(); wake(); ready();
    // MCP pause also waits for the delayed event without waking a conversation.
    selected_mode=0; status(1); app.Run(); reset_observations();
    int wakes=app.protocol_->wakes;
    pause_reported=false;
    delay_hook=[&](int) { if(!pause_reported && elapsed_ms>=5000) { pause_reported=true; status(0); } };
    McpServer::GetInstance().tools.at("self.bl_music.set_play_or_pause")(pause); app.Run(); ready();
    assert(pause_reported && count("pause")==1 && !count("select-tf") && app.protocol_->wakes==wakes);
    // A new music command during query cancels the pending wake before unmuting.
    selected_mode=0; status(1); app.Run(); reset_observations(); wakes=app.protocol_->wakes;
    query_hook=[&] { if(query_count==1) McpServer::GetInstance().tools.at("self.bl_music.set_play_or_pause")(play); };
    wake(); blocked(); assert(app.protocol_->wakes==wakes && count("play")==1);
    // Requests queued before a newer command cannot cancel that newer command.
    reset_observations(); selected_mode=0; status(1); app.Run();
    McpMusic::WakeWord("你好");
    McpServer::GetInstance().tools.at("self.bl_music.set_play_or_pause")(play);
    app.Run(); blocked(); assert(count("play")==1 && !count("pause"));
    reset_observations();
    McpServer::GetInstance().tools.at("self.bl_music.set_play_or_pause")(pause);
    McpServer::GetInstance().tools.at("self.bl_music.set_play_or_pause")(play);
    app.Run(); blocked(); assert(count("play")==1 && !count("pause"));
    // A failed BT switch must not send play to the TF player.
    selected_mode=1; reset_observations(); mode_switch_fails=true;
    McpServer::GetInstance().tools.at("self.bl_music.next")(play); app.Run(); blocked();
    assert(selected_mode==1 && !count("play") && !count("next"));
    // Phone link events immediately close the PCM gate and cancel queued chat.
    reset_observations(); selected_mode=0; status(0); app.Run(); wake(); ready(); reset_observations();
    app.device_state_=kDeviceStateSpeaking; app.protocol_->opened=true;
    app.audio_service_.voice=true;
    uint8_t bt_link=7; // opaque vendor value, never interpreted as play/pause
    emit_event(VB_EVT_BT_STATUS_CHANGE,&bt_link,1);
    blocked(); // flag and hard gate close on UART RX before main-loop cleanup
    app.Run(); blocked();
    assert(app.device_state_==kDeviceStateIdle && !app.audio_service_.voice && !app.protocol_->opened);
    assert(!count("play") && !count("pause")); // phone playback is not controlled on preemption
    // Invalid events cannot stop chat; pausing/disconnecting cannot auto-resume it.
    wake(); ready();
    emit_event(VB_EVT_BT_STATUS_CHANGE,nullptr,0); ready();
    uint8_t tf=1; emit_event(VB_EVT_MODE_CHANGE,&tf,1); ready();
    uint8_t bt=0; emit_event(VB_EVT_MODE_CHANGE,&bt,1); blocked(); app.Run();
    status(0); app.Run(); blocked();
    // Metadata preempts promptly even before delayed 0x0503 notifications.
    wake(); ready(); char title[]="phone track";
    emit_event(VB_EVT_MUSIC_TITLE,title,sizeof(title)); blocked(); app.Run();
    wake(); ready(); char empty[]="";
    emit_event(VB_EVT_MUSIC_LYRC,empty,sizeof(empty)); ready();
    emit_event(VB_EVT_MUSIC_LYRC,title,sizeof(title)); blocked(); app.Run();
    // A frozen/cached position is not playback; an advancing position is.
    wake(); ready(); uint32_t time[]={10,100};
    emit_event(VB_EVT_MUSIC_TIME,time,sizeof(time)); ready();
    emit_event(VB_EVT_MUSIC_TIME,time,sizeof(time)); ready();
    emit_event(VB_EVT_MUSIC_TIME,time,1); ready();
    time[0]=101; emit_event(VB_EVT_MUSIC_TIME,time,sizeof(time)); ready();
    time[0]=11; emit_event(VB_EVT_MUSIC_TIME,time,sizeof(time)); blocked(); app.Run();
    // Background requests run every 250 ms only while chat can run.
    reset_observations(); poll_once(); assert(!count("initial-query"));
    status(0); app.Run(); wake(); ready(); reset_observations();
    poll_once(); assert(count("initial-query")==1);
    uint8_t zero=0,one=1;
    emit_event(VB_EVT_PLAY_STATUS_RESPONSE,&zero,1); ready();
    emit_event(VB_EVT_PLAY_STATUS_RESPONSE,&one,1); blocked(); app.Run();
    poll_once(); assert(count("initial-query")==1);
    // An old polling snapshot must not cancel the user's newer wake request.
    status(0); app.Run(); wake(); ready();
    McpMusic::WakeWord("你好");
    emit_event(VB_EVT_PLAY_STATUS_RESPONSE,&one,1); blocked();
    query_hook=[&] { if(query_count) status(0); }; app.Run(); ready();
    // Phone activity after a queued wake wins before that wake starts pausing music.
    reset_observations(); McpMusic::WakeWord("你好");
    emit_event(VB_EVT_BT_STATUS_CHANGE,&bt_link,1); app.Run(); blocked();
    assert(!count("pause"));
    // Polling is suppressed during synchronous handover queries.
    status(0); app.Run(); reset_observations();
    query_hook=[&] { poll_once(); }; wake(); ready();
    assert(!count("initial-query"));
    // Fast handover needs an actual queried 1 -> 0 transition and consecutive
    // fresh paused replies. All-zero replies alone still cannot reopen PCM.
    auto fresh_music=[&] {
        status(0); app.Run(); status(1); app.Run();
        uint32_t progress[]={10,100}; emit_event(VB_EVT_MUSIC_TIME,progress,sizeof(progress));
        progress[0]=11; emit_event(VB_EVT_MUSIC_TIME,progress,sizeof(progress));
    };
    auto transition=[&] {
        playback_query_value=query_count==1 ? 1 : 0;
        assert(count("pause")== (query_count==1 ? 0 : 1));
    };
    fresh_music(); reset_observations(); query_hook=transition;
    wake(); ready();
    assert(elapsed_ms>=500 && elapsed_ms<1000 && count("pause")==1 && !count("select-tf"));
    // A late progress sample in the initial UART buffer restarts confirmation
    // instead of forcing the whole five-second event wait.
    fresh_music(); reset_observations(); query_hook=transition;
    bool late_progress=false;
    delay_hook=[&](int) {
        if(!late_progress) {
            late_progress=true; uint32_t buffered[]={12,100};
            emit_event(VB_EVT_MUSIC_TIME,buffered,sizeof(buffered));
        }
    };
    wake(); ready(); assert(late_progress && elapsed_ms<1000);
    // A misleading all-zero status stream still requires the real pause event.
    fresh_music(); reset_observations();
    wake(); blocked(); assert(elapsed_ms>=7000 && count("pause")==1);
    // Repeated contradictory progress prevents consecutive paused confirmation.
    fresh_music(); reset_observations(); query_hook=transition;
    uint32_t ongoing[]={11,100}; int next_tick=200;
    delay_hook=[&](int) {
        if(elapsed_ms>=next_tick) {
            ++ongoing[0]; next_tick+=200;
            emit_event(VB_EVT_MUSIC_TIME,ongoing,sizeof(ongoing));
        }
    };
    wake(); blocked(); assert(elapsed_ms>=7000);
    // A newer playing notification overrides an otherwise valid zero stream.
    fresh_music(); reset_observations();
    query_hook=[&] { transition(); if(query_count==3) status(1); };
    wake(); blocked();
    // A position change during PCM drain requires another full confirmation.
    fresh_music(); reset_observations(); query_hook=transition;
    app.audio_service_.during_wait=[&] {
        uint32_t changed[]={1,200}; emit_event(VB_EVT_MUSIC_TIME,changed,sizeof(changed));
    };
    wake(); ready(); assert(elapsed_ms>=750);
    // Without a verified 1 -> 0 transition, retain the delayed-event route.
    status(0); app.Run(); status(1); app.Run(); reset_observations();
    pause_reported=false;
    delay_hook=[&](int) { if(!pause_reported && elapsed_ms>=5000) { pause_reported=true; status(0); } };
    wake(); ready(); assert(pause_reported && elapsed_ms>=5000);
    puts("PASS: sub-second status-transition pause, buffered progress settling, five-second event fallback, contradictory progress/events/drain checks, cancellation, immediate hard gate and phone preemption");
}
'''



def check_driver_bridge(output):
    """Check the actual UART command table and event-forwarding handlers."""
    adapter = (ROOT / "components/vb_adapter/inc/vb_adapter.h").read_text()
    enum_start = adapter.index("typedef enum{")
    enum_end = adapter.index("}VB_EVT_CODE;", enum_start) + len("}VB_EVT_CODE;")
    event_enum = adapter[enum_start:enum_end]
    table = (ROOT / "components/vb_adapter/vb_cmd_evt_table.c").read_text()
    table = "\n".join(line for line in table.splitlines() if not line.startswith("#"))
    handlers = (
        "_on_bt_status_change", "_on_play_status_response", "_on_status_change",
        "_on_mode_change", "_on_music_title", "_on_music_lyrc", "_on_music_time",
    )
    stubs = "\n".join(
        f"int {name}(uint8_t*,uint16_t,void*) {{return 0;}}"
        for name in re.findall(r"int (\w+)\(uint8_t \*data", table)
        if name not in handlers
    )
    code = r"""
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstddef>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
struct vb_cmd_evt_t { uint16_t cmd; int (*evt_cb)(uint8_t*,uint16_t,void*); };
static uint32_t last_event=999;
static std::vector<uint8_t> last_data;
void vb_event_emit(uint32_t event,void* data,uint16_t len) {
    last_event=event; last_data.clear();
    if(data && len) { auto p=static_cast<uint8_t*>(data); last_data.assign(p,p+len); }
}
"""
    code += '#include "' + str(ROOT / "components/vb_adapter/inc/vb_cmd.h") + '"\n'
    code += event_enum + "\n" + stubs + "\n" + table + "\n"
    code += "".join(function("components/vb_adapter/vb_api.c", "int " + name + "(") for name in handlers)
    code += r"""
void dispatch(uint16_t command,uint8_t* data,uint16_t len) {
    for(size_t i=0;i<g_vb_cmd_evt_table_size;++i) {
        if(g_vb_cmd_evt_table[i].cmd==command) { g_vb_cmd_evt_table[i].evt_cb(data,len,nullptr); return; }
    }
    assert(false);
}
int main() {
    static_assert(VB_EVT_STATUS_CHANGE==4 && VB_EVT_PLAY_STATUS_RESPONSE==14 && VB_EVT_BT_STATUS_CHANGE==15);
    uint8_t opaque_bt[]={7,2};
    dispatch(VB_CMD_SYS_BT_STATUS_CHANGE,opaque_bt,sizeof(opaque_bt));
    assert(last_event==VB_EVT_BT_STATUS_CHANGE && last_data==std::vector<uint8_t>({7,2}));
    uint8_t playing=1;
    dispatch(VB_CMD_SYS_PLAY_STATUS_CHANGE,&playing,1);
    assert(last_event==VB_EVT_STATUS_CHANGE);
    dispatch(VB_CMD_GET_PLAY_STATUS,&playing,1);
    assert(last_event==VB_EVT_PLAY_STATUS_RESPONSE);
    dispatch(VB_CMD_SYS_MODE_CHANGE,&playing,1); assert(last_event==VB_EVT_MODE_CHANGE);
    dispatch(VB_CMD_SYS_TITLE,&playing,1); assert(last_event==VB_EVT_MUSIC_TITLE);
    dispatch(VB_CMD_SYS_LYRIC,&playing,1); assert(last_event==VB_EVT_MUSIC_LYRC);
    dispatch(VB_CMD_SYS_TIME,&playing,1); assert(last_event==VB_EVT_MUSIC_TIME);
    dispatch(VB_CMD_SYS_BT_STATUS_CHANGE,nullptr,0);
    assert(last_event==VB_EVT_BT_STATUS_CHANGE && last_data.empty());
    puts("PASS: actual vb_adapter UART table forwards Bluetooth, playback, mode and media events with distinct IDs and unchanged raw payloads");
}
"""
    source = output / "driver_bridge.cc"
    executable = output / "driver_bridge"
    source.write_text(code)
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-fsanitize=address,undefined", "-g", str(source), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)


def main():
    application = "".join(function("main/application.cc", signature) for signature in (
        "void Application::SetChatEnabled(",
        "void Application::ToggleChatState()",
        "void Application::WakeWordInvoke(",
    ))
    header = (ROOT / "firmware/zuowei_v2/mz60-yx-c3/00-mz60-yx-c3/mcp_music.hpp").read_text()
    header = "\n".join(line for line in header.splitlines() if not line.startswith("#"))
    with tempfile.TemporaryDirectory(prefix="mcp-music-source-") as directory:
        output = Path(directory)
        source = output / "controller.cc"
        source.write_text(PREFIX + application + header + MAIN)
        executable = output / "controller"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
        check_driver_bridge(output)


if __name__ == "__main__":
    main()
