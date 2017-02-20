/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
********************************************************************/

#include "ros/names.h"
#include "rosbag/player.h"
#include "rosbag/message_instance.h"
#include "rosbag/view.h"

#if !defined(_MSC_VER)
  #include <sys/select.h>
#endif

#include <boost/foreach.hpp>
#include <boost/format.hpp>

#include "rosgraph_msgs/Clock.h"
#include <algorithm>
#include <rosbag/player.h>

#define foreach BOOST_FOREACH

using std::map;
using std::pair;
using std::string;
using std::vector;
using boost::shared_ptr;
using ros::Exception;

namespace rosbag {

ros::AdvertiseOptions createAdvertiseOptions(const ConnectionInfo* c, uint32_t queue_size, const std::string& prefix) {
    ros::AdvertiseOptions opts(prefix + c->topic, queue_size, c->md5sum, c->datatype, c->msg_def);
    ros::M_string::const_iterator header_iter = c->header->find("latching");
    opts.latch = (header_iter != c->header->end() && header_iter->second == "1");
    return opts;
}


ros::AdvertiseOptions createAdvertiseOptions(MessageInstance const& m, uint32_t queue_size, const std::string& prefix) {
    return ros::AdvertiseOptions(prefix + m.getTopic(), queue_size, m.getMD5Sum(), m.getDataType(), m.getMessageDefinition());
}

// PlayerOptions

PlayerOptions::PlayerOptions() :
    prefix(""),
    quiet(false),
    start_paused(false),
    at_once(false),
    bag_time(false),
    bag_time_frequency(0.0),
    time_scale(1.0),
    queue_size(0),
    advertise_sleep(0.2),
    try_future(false),
    has_time(false),
    loop(false),
    time(0.0f),
    has_duration(false),
    duration(0.0f),
    keep_alive(false),
    skip_empty(ros::DURATION_MAX)
{
}

void PlayerOptions::check() {
    if (bags.size() == 0)
        throw Exception("You must specify at least one bag file to play from");
    if (has_duration && duration <= 0.0)
        throw Exception("Invalid duration, must be > 0.0");
}

// Player

Player::Player(PlayerOptions const& options) :
    options_(options),
    // If we were given a list of topics to pause on, then go into that mode
    // by default (it can be toggled later via 't' from the keyboard).
    pause_for_topics_(options_.pause_topics.size() > 0),
    terminal_modified_(false)
{
    // create inverse remapping map
    for(auto &entry: ros::names::getRemappings()){
        invRemap[entry.second] = entry.first;
        ROS_INFO_STREAM("Remapping from " << entry.first << "=>" << entry.second);
    }
}

Player::~Player() {
    foreach(shared_ptr<Bag> bag, bags_)
        bag->close();

    restoreTerminal();
}

void Player::publish() {
    options_.check();

    // Open all the bag files
    foreach(string const& filename, options_.bags) {
        ROS_INFO("Opening %s", filename.c_str());

        try
        {
            shared_ptr<Bag> bag(boost::make_shared<Bag>());
            bag->open(filename, bagmode::Read);
            bags_.push_back(bag);
        }
        catch (BagUnindexedException ex) {
            std::cerr << "Bag file " << filename << " is unindexed.  Run rosbag reindex." << std::endl;
            return;
        }
    }

    setupTerminal();

    if (!node_handle_.ok())
      return;

    if (!options_.prefix.empty())
    {
      ROS_INFO_STREAM("Using prefix '" << options_.prefix << "'' for topics ");
    }

    if (!options_.quiet)
      puts("");
    
    // Publish all messages in the bags
    View full_view;
    foreach(shared_ptr<Bag> bag, bags_)
        full_view.addQuery(*bag);

    ros::Time initial_time = full_view.getBeginTime();

    initial_time += ros::Duration(options_.time);

    ros::Time finish_time = ros::TIME_MAX;
    if (options_.has_duration)
    {
      finish_time = initial_time + ros::Duration(options_.duration);
    }

    View view;
    TopicQuery topics(options_.topics);

    if (options_.topics.empty())
    {
      foreach(shared_ptr<Bag> bag, bags_)
        view.addQuery(*bag, initial_time, finish_time);
    } else {
      foreach(shared_ptr<Bag> bag, bags_)
        view.addQuery(*bag, topics, initial_time, finish_time);
    }

    if (view.size() == 0)
    {
      std::cerr << "No messages to play on specified topics.  Exiting." << std::endl;
      ros::shutdown();
      return;
    }

    // setup remote control service
    ros::ServiceServer remoteCtrlSrv = node_handle_.advertiseService("bagControl", &Player::remoteCtrlCallback, this);

    // Advertise all of our messages
    foreach(const ConnectionInfo* c, view.getConnections())
    {
        ros::M_string::const_iterator header_iter = c->header->find("callerid");
        std::string callerid = (header_iter != c->header->end() ? header_iter->second : string(""));

        string callerid_topic = callerid + c->topic;

        map<string, ros::Publisher>::iterator pub_iter = publishers_.find(callerid_topic);
        if (pub_iter == publishers_.end()) {

            ros::AdvertiseOptions opts = createAdvertiseOptions(c, options_.queue_size, options_.prefix);

            ros::Publisher pub = node_handle_.advertise(opts);
            publishers_.insert(publishers_.begin(), pair<string, ros::Publisher>(callerid_topic, pub));

            pub_iter = publishers_.find(callerid_topic);
        }
    }

    std::cout << "Waiting " << options_.advertise_sleep.toSec() << " seconds after advertising topics..." << std::flush;
    options_.advertise_sleep.sleep();
    std::cout << " done." << std::endl;

    std::cout << std::endl << "Hit space to toggle paused, or 's' to step." << std::endl;

    throttleDataMap_["USER"] =  {"", (options_.start_paused?0:-1)};

    while (true) {
        // Set up our time_translator and publishers

        time_translator_.setTimeScale(options_.time_scale);

        start_time_ = view.begin()->getTime();
        time_translator_.setRealStartTime(start_time_);
        bag_length_ = view.getEndTime() - view.getBeginTime();

        time_publisher_.setTime(start_time_);

        ros::WallTime now_wt = ros::WallTime::now();
        time_translator_.setTranslatedStartTime(ros::Time(now_wt.sec, now_wt.nsec));


        time_publisher_.setTimeScale(options_.time_scale);
        if (options_.bag_time)
            time_publisher_.setPublishFrequency(options_.bag_time_frequency);
        else
            time_publisher_.setPublishFrequency(-1.0);

        paused_time_ = now_wt;

        // Call do-publish for each message
        foreach(MessageInstance m, view) {
            if (!node_handle_.ok())
                break;

            doPublish(m);
        }

        if (options_.keep_alive)
            while (node_handle_.ok())
                doKeepAlive();

        if (!node_handle_.ok()) {
            std::cout << std::endl;
            break;
        }
        if (!options_.loop) {
            std::cout << std::endl << "Done." << std::endl;
            break;
        }
    }

    ros::shutdown();
}

void Player::printTime(bool pause)
{
    if (!options_.quiet) {

        ros::Time current_time = time_publisher_.getTime();
        ros::Duration d = current_time - start_time_;

        if (pause)
        {
            printf("\r [PAUSED]   Bag Time: %13.6f   Duration: %.6f / %.6f     \r", time_publisher_.getTime().toSec(), d.toSec(), bag_length_.toSec());
        }
        else
        {
            printf("\r [RUNNING]  Bag Time: %13.6f   Duration: %.6f / %.6f     \r", time_publisher_.getTime().toSec(), d.toSec(), bag_length_.toSec());
        }
        fflush(stdout);
    }
}

void Player::doPublish(MessageInstance const& m) {
    string const& topic   = m.getTopic();
    ros::Time const& time = m.getTime();
    string callerid       = m.getCallerId();
    
    ros::Time translated = time_translator_.translate(time);
    horizon_ = ros::WallTime(translated.sec, translated.nsec);


    time_publisher_.setHorizon(time);
    time_publisher_.setWCHorizon(horizon_);

    string callerid_topic = callerid + topic;

    map<string, ros::Publisher>::iterator pub_iter = publishers_.find(callerid_topic);
    ROS_ASSERT(pub_iter != publishers_.end());

    ros::spinOnce();

    // If immediate specified, play immediately
    if (options_.at_once) {
      while(throttlePlayback(m) && node_handle_.ok())
      {
          time_publisher_.runStalledClock(ros::WallDuration(.1));
          ros::spinOnce();
          printTime(true);
      }
      time_publisher_.stepClock();
      pub_iter->second.publish(m);
      updateThrottleData(m);
      printTime();

      return;
    }

    // If skip_empty is specified, skip this region and shift.
    if (time - time_publisher_.getTime() > options_.skip_empty)
    {
      time_publisher_.stepClock();

      ros::WallDuration shift = ros::WallTime::now() - horizon_ ;
      time_translator_.shift(ros::Duration(shift.sec, shift.nsec));
      horizon_ += shift;
      time_publisher_.setWCHorizon(horizon_);
      (pub_iter->second).publish(m);
      updateThrottleData(m);
      printTime();
      return;
    }

    if (pause_for_topics_)
    {
        for (std::vector<std::string>::iterator i = options_.pause_topics.begin();
             i != options_.pause_topics.end();
             ++i)
        {
            if (topic == *i)
            {
                throttleDataMap_["USER"] = {"", 0};
                paused_time_ = ros::WallTime::now();
            }
        }
    }

    do
    {
        ros::spinOnce();

        switch (readCharFromStdin()){
        case ' ':
            toggleUserPause();
            break;
        case 's':
            if (throttlePlayback(m)) {
                time_publisher_.stepClock();

                ros::WallDuration shift = ros::WallTime::now() - horizon_ ;
                paused_time_ = ros::WallTime::now();

                time_translator_.shift(ros::Duration(shift.sec, shift.nsec));

                horizon_ += shift;
                time_publisher_.setWCHorizon(horizon_);

                (pub_iter->second).publish(m);
                updateThrottleData(m);

                printTime(true);
                return;
            }
            break;
        case 't':
            pause_for_topics_ = !pause_for_topics_;
            break;
        };


        if(throttlePlayback(m)){
          printTime(true);
          time_publisher_.runStalledClock(ros::WallDuration(.01));
        }else{
          printTime();
          time_publisher_.runClock(ros::WallDuration(.01));
        }

    }while ((throttlePlayback(m) || !time_publisher_.horizonReached())
            && node_handle_.ok());

    pub_iter->second.publish(m);
    updateThrottleData(m);
}


void Player::doKeepAlive() {
    //Keep pushing ourself out in 10-sec increments (avoids fancy math dealing with the end of time)
    ros::Time const& time = time_publisher_.getTime() + ros::Duration(10.0);

    ros::Time translated = time_translator_.translate(time);
    ros::WallTime horizon = ros::WallTime(translated.sec, translated.nsec);

    time_publisher_.setHorizon(time);
    time_publisher_.setWCHorizon(horizon);

    if (options_.at_once) {
        return;
    }

    while ((throttleDataMap_["USER"].qsize == 0 || !time_publisher_.horizonReached()) && node_handle_.ok())
    {
        bool charsleftorpaused = true;
        while (charsleftorpaused && node_handle_.ok())
        {
            switch (readCharFromStdin()){
            case ' ':
              throttleDataMap_["USER"].qsize = (throttleDataMap_["USER"].qsize?0:-1);
                if (throttleDataMap_["USER"].qsize == 0) {
                    paused_time_ = ros::WallTime::now();
                }
                else
                {
                    ros::WallDuration shift = ros::WallTime::now() - paused_time_;
                    paused_time_ = ros::WallTime::now();
         
                    time_translator_.shift(ros::Duration(shift.sec, shift.nsec));

                    horizon += shift;
                    time_publisher_.setWCHorizon(horizon);
                }
                break;
            case EOF:
                if (throttleDataMap_["USER"].qsize == 0)
                {
                    printTime(true);
                    time_publisher_.runStalledClock(ros::WallDuration(.1));
                }
                else
                    charsleftorpaused = false;
            }
        }

        printTime();
        time_publisher_.runClock(ros::WallDuration(.1));
    }
}



void Player::setupTerminal() {
    if (terminal_modified_)
        return;

#if defined(_MSC_VER)
    input_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (input_handle == INVALID_HANDLE_VALUE)
    {
        std::cout << "Failed to set up standard input handle." << std::endl;
        return;
    }
    if (! GetConsoleMode(input_handle, &stdin_set) )
    {
        std::cout << "Failed to save the console mode." << std::endl;
        return;
    }
    // don't actually need anything but the default, alternatively try this
    //DWORD event_mode = ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    //if (! SetConsoleMode(input_handle, event_mode) )
    //{
    // std::cout << "Failed to set the console mode." << std::endl;
    // return;
    //}
    terminal_modified_ = true;
#else
    const int fd = fileno(stdin);
    termios flags;
    tcgetattr(fd, &orig_flags_);
    flags = orig_flags_;
    flags.c_lflag &= ~ICANON;      // set raw (unset canonical modes)
    flags.c_cc[VMIN]  = 0;         // i.e. min 1 char for blocking, 0 chars for non-blocking
    flags.c_cc[VTIME] = 0;         // block if waiting for char
    tcsetattr(fd, TCSANOW, &flags);

    FD_ZERO(&stdin_fdset_);
    FD_SET(fd, &stdin_fdset_);
    maxfd_ = fd + 1;
    terminal_modified_ = true;
#endif
}

void Player::restoreTerminal() {
	if (!terminal_modified_)
		return;

#if defined(_MSC_VER)
    SetConsoleMode(input_handle, stdin_set);
#else
    const int fd = fileno(stdin);
    tcsetattr(fd, TCSANOW, &orig_flags_);
#endif
    terminal_modified_ = false;
}

int Player::readCharFromStdin() {
#ifdef __APPLE__
    fd_set testfd;
    FD_COPY(&stdin_fdset_, &testfd);
#elif !defined(_MSC_VER)
    fd_set testfd = stdin_fdset_;
#endif

#if defined(_MSC_VER)
    DWORD events = 0;
    INPUT_RECORD input_record[1];
    DWORD input_size = 1;
    BOOL b = GetNumberOfConsoleInputEvents(input_handle, &events);
    if (b && events > 0)
    {
        b = ReadConsoleInput(input_handle, input_record, input_size, &events);
        if (b)
        {
            for (unsigned int i = 0; i < events; ++i)
            {
                if (input_record[i].EventType & KEY_EVENT & input_record[i].Event.KeyEvent.bKeyDown)
                {
                    CHAR ch = input_record[i].Event.KeyEvent.uChar.AsciiChar;
                    return ch;
                }
            }
        }
    }
    return EOF;
#else
    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    if (select(maxfd_, &testfd, NULL, NULL, &tv) <= 0)
        return EOF;
    return getc(stdin);
#endif
}

bool Player::remoteCtrlCallback(rc_bagthrottler::ThrottleBag::Request &req,
                        rc_bagthrottler::ThrottleBag::Request &res){
  if(req.id.size()){
    if(req.id == "USER"
       && ((req.qsize == 0) != (throttleDataMap_["USER"].qsize == 0))){
      toggleUserPause();
      throttleDataMap_["USER"].qsize = req.qsize;
    }else{
      auto remapEntry = invRemap.find(req.topic);
      std::string topic;
      if(remapEntry != invRemap.end()){
          topic = remapEntry->second;
      }else{
          topic = req.topic; 
      }
      throttleDataMap_[req.id] = {topic, throttleDataMap_[req.id].qsize + req.qsize};
    }
  }else{
    // Print queuing information
    ROS_INFO_STREAM("\nThrottle queue:");
    for(auto &entry: throttleDataMap_){
      ROS_INFO_STREAM("Queue space for subscriber \""
                              << entry.first << "\" on topic \""
                              << entry.second.topic << "\": "
                              << entry.second.qsize);
    }
  }

  return true;
}

bool Player::throttlePlayback(rosbag::MessageInstance const &m)
{
  bool userPause = (throttleDataMap_["USER"].qsize == 0);
  bool topicPause =
          std::any_of(throttleDataMap_.begin(), throttleDataMap_.end(),
                      [m](std::pair<const std::string, throttleData_t> &entry)
                         {return (entry.second.qsize==0)
                                    && (entry.second.topic == m.getTopic());});
  return userPause || topicPause;
}

void Player::updateThrottleData(rosbag::MessageInstance const& m){
  for(auto &entry: throttleDataMap_){
    if(entry.second.topic == m.getTopic())
    {
      if (entry.second.qsize > 0)
      {
        entry.second.qsize--;
      }
    }else if( entry.first == "USER" ){
      if(entry.second.qsize > 1){
        entry.second.qsize--;
      }else if(entry.second.qsize == 1){
        toggleUserPause();
      }
    }
  }
}

void Player::toggleUserPause()
{

  // toggle run state
  throttleDataMap_["USER"].qsize = (throttleDataMap_["USER"].qsize?0:-1);

  if(throttleDataMap_["USER"].qsize == 0){
    paused_time_ = ros::WallTime::now();
  }else{
    ros::WallDuration shift = ros::WallTime::now() - paused_time_;
    time_translator_.shift(ros::Duration(shift.sec, shift.nsec));
    horizon_ += shift;
    time_publisher_.setWCHorizon(horizon_);
  }

}

TimePublisher::TimePublisher() : time_scale_(1.0)
{
  setPublishFrequency(-1.0);
  time_pub_ = node_handle_.advertise<rosgraph_msgs::Clock>("clock",1);
}

void TimePublisher::setPublishFrequency(double publish_frequency)
{
  publish_frequency_ = publish_frequency;
  
  do_publish_ = (publish_frequency > 0.0);

  wall_step_.fromSec(1.0 / publish_frequency);
}

void TimePublisher::setTimeScale(double time_scale)
{
    time_scale_ = time_scale;
}

void TimePublisher::setHorizon(const ros::Time& horizon)
{
  horizon_ = horizon;
}

void TimePublisher::setWCHorizon(const ros::WallTime& horizon)
{
  wc_horizon_ = horizon;
}

void TimePublisher::setTime(const ros::Time& time)
{
    current_ = time;
}

ros::Time const& TimePublisher::getTime() const
{
    return current_;
}

void TimePublisher::runClock(const ros::WallDuration& duration)
{
    if (do_publish_)
    {
        rosgraph_msgs::Clock pub_msg;

        ros::WallTime t = ros::WallTime::now();
        ros::WallTime done = t + duration;

        do
        {
            ros::WallDuration leftHorizonWC = wc_horizon_ - t;
            ros::Duration d(leftHorizonWC.sec, leftHorizonWC.nsec);
            d *= time_scale_;

            current_ = horizon_ - d;

            if (current_ >= horizon_)
              current_ = horizon_;

            if (t >= next_pub_)
            {
                pub_msg.clock = current_;
                time_pub_.publish(pub_msg);
                next_pub_ = t + wall_step_;
            }

            ros::WallTime target = done;
            if (target > wc_horizon_)
              target = wc_horizon_;
            if (target > next_pub_)
              target = next_pub_;

            ros::WallTime::sleepUntil(target);

            t = ros::WallTime::now();
        }while (t < done && t < wc_horizon_);

    } else {

        ros::WallTime t = ros::WallTime::now();

        ros::WallDuration leftHorizonWC = wc_horizon_ - t;

        ros::Duration d(leftHorizonWC.sec, leftHorizonWC.nsec);
        d *= time_scale_;

        current_ = horizon_ - d;
        
        if (current_ >= horizon_)
            current_ = horizon_;

        ros::WallTime target = ros::WallTime::now() + duration;

        if (target > wc_horizon_)
            target = wc_horizon_;

        ros::WallTime::sleepUntil(target);
    }
}

void TimePublisher::stepClock()
{
    if (do_publish_)
    {
        current_ = horizon_;

        rosgraph_msgs::Clock pub_msg;

        pub_msg.clock = current_;
        time_pub_.publish(pub_msg);

        ros::WallTime t = ros::WallTime::now();
        next_pub_ = t + wall_step_;
    } else {
        current_ = horizon_;
    }
}

void TimePublisher::runStalledClock(const ros::WallDuration& duration)
{
    if (do_publish_)
    {
        rosgraph_msgs::Clock pub_msg;

        ros::WallTime t = ros::WallTime::now();
        ros::WallTime done = t + duration;

        while ( t < done )
        {
            if (t > next_pub_)
            {
                pub_msg.clock = current_;
                time_pub_.publish(pub_msg);
                next_pub_ = t + wall_step_;
            }

            ros::WallTime target = done;

            if (target > next_pub_)
              target = next_pub_;

            ros::WallTime::sleepUntil(target);

            t = ros::WallTime::now();
        }
    } else {
        duration.sleep();
    }
}

bool TimePublisher::horizonReached()
{
  return ros::WallTime::now() > wc_horizon_;
}

} // namespace rosbag
