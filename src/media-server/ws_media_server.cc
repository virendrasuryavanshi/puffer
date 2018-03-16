#include <cstdint>

#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <random>

#include "yaml.hh"
#include "inotify.hh"
#include "timerfd.hh"
#include "channel.hh"
#include "message.hh"
#include "ws_server.hh"
#include "ws_client.hh"

using namespace std;
using namespace PollerShortNames;

const int DEFAULT_MAX_BUFFER_S = 60;
const int DEFAULT_MAX_INFLIGHT_S = 5;
const size_t DEFAULT_MAX_WS_FRAME_LEN = 100000;
const size_t DEFAULT_MAX_WS_QUEUE_LEN = DEFAULT_MAX_WS_FRAME_LEN;

static vector<string> channel_names;
static map<string, Channel> channels;
static map<uint64_t, WebSocketClient> clients;

static Timerfd global_timer;

static unsigned int max_buffer_seconds;
static unsigned int max_inflight_seconds;

static size_t max_ws_frame_len;
static size_t max_ws_queue_len;

void print_usage(const string & program_name)
{
  cerr << program_name << " <YAML configuration>" << endl;
}

inline int randint(const int a, const int b)
{
  assert(a < b);
  int ret = a + rand() % (b - a);
  assert(ret >= a and ret < b);
  return ret;
}

const VideoFormat & select_video_quality(WebSocketClient & client)
{
  // TODO: make a real choice
  Channel & channel = channels.at(client.channel().value());
  // return client.curr_vq().value_or(channel.vformats()[0]);
  return channel.vformats()[randint(0, channel.vformats().size())];
}

const AudioFormat & select_audio_quality(WebSocketClient & client)
{
  // TODO: make a real choice
  Channel & channel = channels.at(client.channel().value());
  // return client.curr_aq().value_or(channel.aformats()[0]);
  return channel.aformats()[randint(0, channel.aformats().size())];
}

void serve_video_to_client(WebSocketServer & server, WebSocketClient & client)
{
  Channel & channel = channels.at(client.channel().value());

  uint64_t next_vts = client.next_vts().value();

  if (!client.next_vsegment().has_value()) { /* or try a lower quality */
    /* Start new chunk */
    if (not channel.vready(next_vts)) {
      return;
    }
    const VideoFormat & next_vq = select_video_quality(client);

    cerr << "serving (id=" << client.connection_id() << ") video " << next_vts
         << " " << next_vq << endl;

    optional<mmap_t> init_mmap;
    if (not client.curr_vq().has_value() or
        next_vq != client.curr_vq().value()) {
      init_mmap = channel.vinit(next_vq);
    }
    client.set_next_vsegment(next_vq, channel.vdata(next_vq, next_vts),
                             init_mmap);
  } else {
    cerr << "continuing (id=" << client.connection_id() << ") video "
         << next_vts << endl;
  }

  VideoSegment & next_vsegment = client.next_vsegment().value();

  string frame_payload = make_video_msg(next_vsegment.format().to_string(),
                                        next_vts, channel.vduration(),
                                        next_vsegment.offset(),
                                        next_vsegment.length());
  frame_payload.append(next_vsegment.read(max_ws_frame_len));

  WSFrame frame {true, WSFrame::OpCode::Binary, frame_payload};
  server.queue_frame(client.connection_id(), frame);

  if (next_vsegment.done()) {
    client.set_next_vts(next_vts + channel.vduration());
    client.set_curr_vq(next_vsegment.format());
    client.clear_next_vsegment();
  }
}

void serve_audio_to_client(WebSocketServer & server, WebSocketClient & client)
{
  Channel & channel = channels.at(client.channel().value());
  uint64_t next_ats = client.next_ats().value();

  if (not client.next_asegment().has_value()) { /* or try a lower quality */
    if (not channel.aready(next_ats)) {
      return;
    }

    const AudioFormat & next_aq = select_audio_quality(client);

    cerr << "serving (id=" << client.connection_id() << ") audio " << next_ats
         << " " << next_aq << endl;

    optional<mmap_t> init_mmap;
    if (not client.curr_aq().has_value() or
        next_aq != client.curr_aq().value()) {
      init_mmap = channel.ainit(next_aq);
    }
    client.set_next_asegment(next_aq, channel.adata(next_aq, next_ats),
                             init_mmap);
  } else {
    cerr << "continuing (id=" << client.connection_id() << ") audio "
         << next_ats << endl;
  }

  AudioSegment & next_asegment = client.next_asegment().value();

  string frame_payload = make_audio_msg(next_asegment.format().to_string(),
                                        next_ats,
                                        channel.aduration(),
                                        next_asegment.offset(),
                                        next_asegment.length());
  frame_payload.append(next_asegment.read(max_ws_frame_len));

  WSFrame frame {true, WSFrame::OpCode::Binary, frame_payload};
  server.queue_frame(client.connection_id(), frame);

  if (next_asegment.done()) {
    client.set_next_ats(next_ats + channel.aduration());
    client.set_curr_aq(next_asegment.format());
    client.clear_next_asegment();
  }
}

inline unsigned int video_in_flight(const Channel & channel,
                                    const WebSocketClient & client)
{
  /* Return number of seconds of video in flight */
  return (client.next_vts().value() - client.client_next_vts().value())
          / channel.timescale();
}

inline unsigned int audio_in_flight(const Channel & channel,
                                    const WebSocketClient & client)
{
  /* Return number of seconds of audio in flight */
  return (client.next_ats().value() - client.client_next_ats().value())
          / channel.timescale();
}

void serve_client(WebSocketServer & server, WebSocketClient & client)
{
  const Channel & channel = channels.at(client.channel().value());
  if (server.queue_size(client.connection_id()) < max_ws_queue_len) {
    const bool can_send_video =
      client.video_playback_buf() < max_buffer_seconds and
      video_in_flight(channel, client) < max_inflight_seconds;
    const bool can_send_audio =
      client.audio_playback_buf() < max_buffer_seconds and
      audio_in_flight(channel, client) < max_inflight_seconds;

    if (client.next_vts().value() > client.next_ats().value()) {
      /* prioritize audio */
      if (can_send_audio) {
        serve_audio_to_client(server, client);
      }
      /* serve video only if there is still room */
      if (can_send_video and
          server.queue_size(client.connection_id()) < max_ws_queue_len) {
        serve_video_to_client(server, client);
      }
    } else {
      /* prioritize video */
      if (can_send_video) {
        serve_video_to_client(server, client);
      }
      /* serve audio only if there is still room */
      if (can_send_audio and
          server.queue_size(client.connection_id()) < max_ws_queue_len) {
        serve_audio_to_client(server, client);
      }
    }
  }
}

void start_global_timer(WebSocketServer & server)
{
  /* the timer fires every 10 ms */
  global_timer.start(10, 10);

  server.poller().add_action(
    Poller::Action(global_timer, Direction::In,
      [&]() {
        if (global_timer.expirations() > 0) {
          /* iterate over all connections */
          for (auto & client_item : clients) {
            WebSocketClient & client = client_item.second;
            if (client.channel().has_value()) {
              serve_client(server, client);
            }
          }
        }

        return ResultType::Continue;
      }
    )
  );
}

void handle_client_init(WebSocketServer & server, WebSocketClient & client,
                        const ClientInitMessage & message)
{
  auto it = message.channel.has_value() ?
    channels.find(message.channel.value()) : channels.begin();
  if (it == channels.end()) {
    throw BadClientMessageException("Requested channel not found");
  }
  auto & channel = it->second;

  uint16_t init_vts = channel.init_vts();
  uint16_t init_ats = channel.find_ats(init_vts);

  client.init(channel.name(), init_vts, init_ats);

  string reply = make_server_init_msg(channel.name(), channel.vcodec(),
                                      channel.acodec(), channel.timescale(),
                                      client.next_vts().value(),
                                      client.init_id());

  /* Reinitialize video playback on the client */
  WSFrame frame {true, WSFrame::OpCode::Binary, reply};
  server.queue_frame(client.connection_id(), frame);
}

void handle_client_info(WebSocketClient & client,
                        const ClientInfoMessage & message)
{
  if (message.init_id == client.init_id()) {
    client.set_audio_playback_buf(message.audio_buffer_len);
    client.set_video_playback_buf(message.video_buffer_len);
    client.set_client_next_vts(message.next_video_timestamp);
    client.set_client_next_ats(message.next_audio_timestamp);
  }
}

void handle_client_open(WebSocketServer & server, const uint64_t connection_id)
{
  /* Send the client the list of playable channels */
  string server_hello = make_server_hello_msg(channel_names);
  WSFrame frame {true, WSFrame::OpCode::Binary, server_hello};
  server.queue_frame(connection_id, frame);
}

int main(int argc, char * argv[])
{
  if (argc < 1) {
    abort();
  }

  if (argc != 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  YAML::Node config = load_yaml_unsafe(argv[1]);

  /* create a WebSocketServer instance */
  const string ip = "0.0.0.0";
  const uint16_t port = config["port"].as<int>();
  WebSocketServer server {{ip, port}};

  /* mmap new media files */
  Inotify inotify(server.poller());

  for (YAML::const_iterator it = config["channel"].begin();
       it != config["channel"].end(); ++it) {
    const string channel_name = it->as<string>();
    channels.emplace(channel_name,
                     Channel(channel_name, config[channel_name], inotify));
    channel_names.push_back(channel_name);
  }

  max_buffer_seconds = config["max_buffer_s"] ?
    config["max_buffer_s"].as<int>() : DEFAULT_MAX_BUFFER_S;
  max_inflight_seconds = config["max_inflight_s"] ?
    config["max_inflight_s"].as<int>() : DEFAULT_MAX_INFLIGHT_S;
  max_ws_frame_len = config["max_ws_frame_b"] ?
    config["max_ws_frame_b"].as<int>() : DEFAULT_MAX_WS_FRAME_LEN;
  max_ws_queue_len = config["max_ws_queue_b"] ?
    config["max_ws_queue_b"].as<int>() : DEFAULT_MAX_WS_QUEUE_LEN;

  /* start the global timer */
  start_global_timer(server);

  server.set_message_callback(
    [&](const uint64_t connection_id, const WSMessage & message)
    {
      cerr << "Message (from=" << connection_id << "): "
           << message.payload() << endl;

      WebSocketClient & client = clients.at(connection_id);

      try {
        const auto data = unpack_client_msg(message.payload());
        switch (data.first) {
          case ClientMessageType::Init: {
            ClientInitMessage client_init = parse_client_init_msg(data.second);
            handle_client_init(server, client, client_init);
            break;
          }
          case ClientMessageType::Info: {
            ClientInfoMessage client_info = parse_client_info_msg(data.second);
            handle_client_info(client, client_info);
            break;
          }
          default:
            break;
        }
      } catch (const BadClientMessageException & e) {
        cerr << "Bad message from client: " << e.what() << endl;
        clients.erase(connection_id);
      }
    }
  );

  server.set_open_callback(
    [&](const uint64_t connection_id)
    {
      cerr << "Connected (id=" << connection_id << ")" << endl;

      handle_client_open(server, connection_id);
      auto ret = clients.emplace(connection_id, WebSocketClient(connection_id));
      if (not ret.second) {
        throw runtime_error("Connection ID " + to_string(connection_id) +
                            " already exists");
      }
    }
  );

  server.set_close_callback(
    [&](const uint64_t connection_id)
    {
      cerr << "Connection closed (id=" << connection_id << ")" << endl;

      clients.erase(connection_id);
    }
  );

  for (;;) {
    /* TODO: returns Poller::Result::Type::Exit sometimes? */
    server.loop_once();
  }

  return EXIT_SUCCESS;
}