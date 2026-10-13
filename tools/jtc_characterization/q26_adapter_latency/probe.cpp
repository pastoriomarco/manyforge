// Evidence-only real-adapter measurement; not a product controller or Stop law.
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
using namespace manyforge;
using Action = control_msgs::action::FollowJointTrajectory;
using StateMessage = control_msgs::msg::JointTrajectoryControllerState;
using FeedbackMessage = Action::Impl::FeedbackMessage;
using namespace std::chrono_literals;
struct Clock final : core::IClock {
  core::TimePoint now() const override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
};
struct Sink final : core::IExecutionStateSink {
  core::ExecutionStateSinkResult apply_group_joint_positions(const std::string&, const std::vector<double>&, core::ExecutionStateUpdateMode) override { return {}; }
  core::ExecutionStateSinkResult apply_group_joint_position(const std::string&, const std::string&, double, core::ExecutionStateUpdateMode) override { return {}; }
  std::optional<double> group_joint_position(const std::string&, const std::string&) const override { return std::nullopt; }
};
template<class T> std::int64_t ns(const T& t) { return static_cast<std::int64_t>(t.sec)*1000000000+t.nanosec; }
std::string uuid(const auto& id) {
  std::ostringstream out; out << std::hex << std::setfill('0');
  for (auto b:id) out << std::setw(2) << static_cast<unsigned>(b);
  return out.str();
}
struct Sample {
  std::int64_t local, source, cursor;
  double q,v,a,output,factor;
  std::string id;
};
struct Observers {
  Clock& clock;
  std::mutex mutex;
  std::vector<Sample> states, feedback;
  std::atomic<bool> overflow{false};
  explicit Observers(Clock& c):clock(c) { states.reserve(30000);feedback.reserve(30000); }
  void store(std::vector<Sample>& values, Sample sample) {
    std::lock_guard lock(mutex);
    if(values.size()>=30000) { overflow=true; return; }
    values.push_back(std::move(sample));
  }
  void state(const StateMessage& m) {
    auto local=clock.now();
    if(m.joint_names!=std::vector<std::string>{"joint1"} || m.reference.positions.size()!=1 || m.reference.velocities.size()!=1 || m.reference.accelerations.size()!=1 || m.output.positions.size()!=1) return;
    store(states,{local,ns(m.header.stamp),ns(m.reference.time_from_start),m.reference.positions[0],m.reference.velocities[0],m.reference.accelerations[0],m.output.positions[0],m.speed_scaling_factor,{}});
  }
  void action(const FeedbackMessage& m) {
    auto local=clock.now(); const auto& f=m.feedback;
    if(f.joint_names!=std::vector<std::string>{"joint1"} || f.desired.positions.size()!=1 || f.desired.velocities.size()!=1 || f.desired.accelerations.size()!=1) return;
    store(feedback,{local,ns(f.header.stamp),ns(f.desired.time_from_start),f.desired.positions[0],f.desired.velocities[0],f.desired.accelerations[0],0,0,uuid(m.goal_id.uuid)});
  }
  std::optional<Sample> latest_for(const std::string& id) {
    std::lock_guard lock(mutex);
    for(auto s=states.rbegin();s!=states.rend();++s) {
      for(auto f=feedback.rbegin();f!=feedback.rend();++f) {
        if(f->id==id && f->source==s->source && f->cursor==s->cursor && f->q==s->q && f->v==s->v && f->a==s->a) return *s;
      }
    }
    return std::nullopt;
  }
  std::optional<Sample> correlate(std::int64_t begin, const Sample& prior, const std::string& id) {
    std::lock_guard lock(mutex);
    // Same source stamp + exact own-UUID desired PVA. Never infer from acceptance.
    for(const auto& s:states) {
      if(s.local<begin || s.source<=prior.source || s.cursor>=prior.cursor) continue;
      for(const auto& f:feedback) {
        if(f.local>=begin && f.id==id && f.source==s.source && f.cursor==s.cursor && f.q==s.q && f.v==s.v && f.a==s.a) return s;
      }
    }
    return std::nullopt;
  }
};
void write_sample(std::ostream& out,const Sample& s,const char* kind) {
  out << "{\"kind\":\""<<kind<<"\",\"local_ns\":"<<s.local<<",\"source_ns\":"<<s.source<<",\"cursor_ns\":"<<s.cursor<<",\"q\":"<<s.q<<",\"v\":"<<s.v<<",\"a\":"<<s.a<<",\"output_q\":"<<s.output<<",\"factor\":"<<s.factor<<",\"uuid\":\""<<s.id<<"\"}";
}
core::ExecutionGoalKey key(std::uint64_t generation) {
  return {core::ControllerGoalId("q26.fixture.arm"),core::ControllerGoalGeneration(generation)};
}
core::TrajectoryDTO trajectory(double offset) {
  core::TrajectoryDTO t; t.joint_names={"joint1"};
  t.waypoints={{-offset,{0},{.02},{0}}, {100-offset,{2},{.02},{0}}, {101-offset,{2.01},{0},{0}}};
  return t;
}
template<class F> bool wait(F condition, std::chrono::seconds budget=3s) {
  auto end=std::chrono::steady_clock::now()+budget;
  while(std::chrono::steady_clock::now()<end) { if(condition()) return true; std::this_thread::sleep_for(1ms); }
  return condition();
}
}
int main(int argc,char** argv) {
  if(argc!=5) return 2;
  const int rate=std::stoi(argv[1]), samples=std::stoi(argv[2]), warmups=std::stoi(argv[3]);
  std::ofstream records(std::string(argv[4])+"/replacements.jsonl"),raw(std::string(argv[4])+"/raw-observations.jsonl");
  records<<std::setprecision(17);raw<<std::setprecision(17);
  Clock clock; Observers observer(clock);
  ros2_control_execution::Ros2TransportResources resources("q26_real_adapter","",2,std::make_shared<Sink>(),"arm",clock);
  ros2_control_execution::JtcExecutionAdapterConfig cfg;
  cfg.action_name="/arm/follow_joint_trajectory";cfg.state_topic="/arm/controller_state";cfg.scale_topic="/arm/speed_scaling_input";
  cfg.binding_epoch=core::ExecutionBindingEpoch(1);cfg.lane_facts.joint_names={"joint1"};cfg.lane_facts.supports_scale=true;
  cfg.lane_facts.has_measured_velocity=true;cfg.lane_facts.controller_update_rate_hz=rate;
  auto adapter=std::make_unique<ros2_control_execution::Ros2ControlExecutionBackend>(resources,clock,cfg);
  auto state_sub=resources.node()->create_subscription<StateMessage>(cfg.state_topic,rclcpp::QoS(2000).reliable(),[&](const StateMessage& m){observer.state(m);});
  auto feedback_sub=resources.node()->create_subscription<FeedbackMessage>(cfg.action_name+"/_action/feedback",rclcpp::QoS(2000).reliable(),[&](const FeedbackMessage& m){observer.action(m);});
  adapter->ObserveReadiness({true,true,true,core::ExecutionBindingEpoch(1)});
  resources.start();int exit_code=0;
  try {
    core::ExecutionPortCommandResultDTO initial;
    bool submitted=wait([&] {
      initial=adapter->Submit(key(1),trajectory(0));
      if(initial.dispatch!=core::ExecutionPortDispatch::kNotDispatched) return true;
      if(initial.refusal!=core::ExecutionPortRefusal::kNotReady) throw std::runtime_error("initial Submit refused");
      return false;
    },5s);
    if(!submitted || initial.dispatch!=core::ExecutionPortDispatch::kDispatched) throw std::runtime_error("initial dispatch unavailable/uncertain");
    if(!wait([&]{auto g=adapter->ReadGoal(key(1));return g && g->accepted_stamp_ns && g->latest_feedback;})) throw std::runtime_error("initial acceptance/feedback timeout");
    double offset=0;auto active=key(1);
    for(int i=0;i<samples+warmups;++i) {
      std::optional<Sample> matched_prior;
      if(!wait([&]{
        auto g=adapter->ReadGoal(active);
        if(!g || g->phase==core::ExecutionGoalPhase::kTerminal || g->mechanism_goal_id.empty()) return false;
        matched_prior=observer.latest_for(g->mechanism_goal_id.value());
        return matched_prior && matched_prior->cursor>=100000000 && std::abs(matched_prior->v-.02)<1e-10;
      })) throw std::runtime_error("own-UUID moving predecessor timeout");
      auto prior=*matched_prior;auto previous=adapter->ReadGoal(active);
      if(!previous || previous->phase==core::ExecutionGoalPhase::kTerminal || prior.cursor<100000000 || std::abs(prior.v-.02)>1e-10 || prior.factor!=1) throw std::runtime_error("invalid moving predecessor");
      offset+=static_cast<double>(prior.cursor)/1e9;
      auto next=key(static_cast<std::uint64_t>(i)+2);auto sent=trajectory(offset);
      auto begin=clock.now();auto command=adapter->Replace(active,next,sent);auto returned=clock.now();
      std::optional<Sample> consumed;std::optional<core::ExecutionGoalObservationDTO> goal;
      bool correlated=false;
      if(command.dispatch==core::ExecutionPortDispatch::kDispatched) correlated=wait([&]{
        goal=adapter->ReadGoal(next);
        if(!goal || !goal->accepted_stamp_ns || goal->mechanism_goal_id.empty()) return false;
        consumed=observer.correlate(begin,prior,goal->mechanism_goal_id.value());return consumed.has_value();
      });
      records<<"{\"index\":"<<i<<",\"warmup\":"<<(i<warmups?"true":"false")<<",\"replace_entry_ns\":"<<begin<<",\"replace_return_ns\":"<<returned<<",\"dispatch\":"<<static_cast<int>(command.dispatch)<<",\"correlated\":"<<(correlated?"true":"false")<<",\"offset_s\":"<<offset<<",\"predecessor_uuid\":\""<<previous->mechanism_goal_id<<"\",\"uuid\":\""<<(goal?goal->mechanism_goal_id.value():"")<<"\",\"accept_receipt_ns\":"<<(goal && goal->accepted_stamp_ns?*goal->accepted_stamp_ns:0)<<",\"predecessor\":";
      write_sample(records,prior,"state");records<<",\"first_correlated_state\":";
      if(consumed) write_sample(records,*consumed,"state");else records<<"null";
      records<<",\"signed_waypoints\":[";
      for(std::size_t j=0;j<sent.waypoints.size();++j) {if(j)records<<',';const auto& p=sent.waypoints[j];records<<"{\"t\":"<<p.time_from_start_s<<",\"p\":"<<p.positions_rad[0]<<",\"v\":"<<p.velocities_rad_s[0]<<",\"a\":"<<p.accelerations_rad_s2[0]<<'}';}
      records<<"]}"<<std::endl;
      if(!correlated) throw std::runtime_error("measured replacement uncorrelated/refused/uncertain; no retry");
      if(!wait([&]{auto g=adapter->ReadGoal(active);return g && g->phase==core::ExecutionGoalPhase::kTerminal;})) throw std::runtime_error("predecessor terminal timeout; no third record");
      if(observer.overflow) throw std::runtime_error("bounded observer overflow");
      active=next;
    }
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';exit_code=1;}
  adapter->shutdown();resources.quiesce();adapter.reset();
  for(const auto& s:observer.states){write_sample(raw,s,"state");raw<<'\n';}
  for(const auto& s:observer.feedback){write_sample(raw,s,"feedback");raw<<'\n';}
  std::cout<<"observer_states="<<observer.states.size()<<" observer_feedback="<<observer.feedback.size()<<" overflow="<<observer.overflow<<" exit="<<exit_code<<'\n';
  return exit_code;
}
