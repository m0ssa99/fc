#include <fc/network/ntp.hpp>
#include <fc/network/udp_socket.hpp>
#include <fc/network/resolve.hpp>
#include <fc/network/ip.hpp>
#include <fc/thread/thread.hpp>

#include <cstdint>
#include "../byteswap.hpp"

#include <atomic>
#include <array>
#include <deque>
#include <algorithm>

namespace fc
{
  namespace detail {

  class ntp_impl
  {
    public:
      /** vector < host, port >  */
      fc::thread                                       _ntp_thread;
      std::vector< std::pair< std::string, uint16_t> > _ntp_hosts;
      fc::future<void>                                 _read_loop_done;
      udp_socket                                       _sock;
      uint32_t                                         _request_interval_sec;
      uint32_t                                         _retry_failed_request_interval_sec;
      fc::time_point                                   _last_valid_ntp_reply_received_time;

      std::atomic_bool                                 _last_ntp_delta_initialized;
      std::atomic<int64_t>                             _last_ntp_delta_microseconds;


      fc::future<void>                                 _request_time_task_done;

      bool                                               _valid_reply_received_this_cycle = false;
      fc::time_point                                       _last_request_sent_time;

      size_t                                           _delta_history_max_size;
      std::deque<int64_t>                              _delta_history;
      uint32_t                                         _round_trip_threshold_us;
      uint32_t                                         _rejection_threshold_pct;
      uint32_t                                         _rejection_min_threshold_us;

      ntp_impl() :
      _ntp_thread("ntp"),
      _request_interval_sec( 15*60 /* 15 min */),
      _retry_failed_request_interval_sec(60 * 5),
      _last_ntp_delta_microseconds(0),
      _delta_history_max_size(5),
      _round_trip_threshold_us(150000),
      _rejection_threshold_pct(50),
      _rejection_min_threshold_us(5000)
      {
        _last_ntp_delta_initialized = false;
        _ntp_hosts.push_back( std::make_pair( "pool.ntp.org", 123 ) );
        _ntp_hosts.push_back( std::make_pair( "time.google.com", 123 ) );
        _ntp_hosts.push_back( std::make_pair( "time.cloudflare.com", 123 ) );
      }

      ~ntp_impl()
      {
      }

      fc::time_point ntp_timestamp_to_fc_time_point(uint64_t ntp_timestamp_net_order)
      {
        uint64_t ntp_timestamp_host = bswap_64(ntp_timestamp_net_order);
        uint32_t fractional_seconds = ntp_timestamp_host & 0xffffffff;
        uint32_t microseconds = (uint32_t)((((uint64_t)fractional_seconds * 1000000) + (uint64_t(1) << 31)) >> 32);
        uint32_t seconds_since_1900 = ntp_timestamp_host >> 32;
        uint32_t seconds_since_epoch = seconds_since_1900 - 2208988800;
        return fc::time_point() + fc::seconds(seconds_since_epoch) + fc::microseconds(microseconds);
      }

      uint64_t fc_time_point_to_ntp_timestamp(const fc::time_point& fc_timestamp)
      {
        uint64_t microseconds_since_epoch = (uint64_t)fc_timestamp.time_since_epoch().count();
        uint32_t seconds_since_epoch = (uint32_t)(microseconds_since_epoch / 1000000);
        uint32_t seconds_since_1900 = seconds_since_epoch + 2208988800;
        uint32_t microseconds = microseconds_since_epoch % 1000000;
        uint32_t fractional_seconds = (uint32_t)((((uint64_t)microseconds << 32) + (uint64_t(1) << 31)) / 1000000);
        uint64_t ntp_timestamp_net_order = ((uint64_t)seconds_since_1900 << 32) + fractional_seconds;
        return bswap_64(ntp_timestamp_net_order);
      }

      void request_now()
      {
        assert(_ntp_thread.is_current());
        // Rate-limit: don't re-send if we already sent a request within the last 3 seconds
        // This prevents a feedback loop where stale replies each trigger new requests
        auto now = fc::time_point::now();
        if( now - _last_request_sent_time < fc::seconds(3) )
          return;
        _last_request_sent_time = now;
        _valid_reply_received_this_cycle = false;
        for( size_t i = 0; i < _ntp_hosts.size(); )
        {
          auto& item = _ntp_hosts[i];
          try
          {
            //wlog( "resolving... ${r}", ("r", item) );
            auto eps = resolve( item.first, item.second );
            for( auto ep : eps )
            {
             // wlog( "sending request to ${ep}", ("ep",ep) );
              std::shared_ptr<char> send_buffer(new char[48], [](char* p){ delete[] p; });
              std::array<unsigned char, 48> packet_to_send { {010,0,0,0,0,0,0,0,0} };
              memcpy(send_buffer.get(), packet_to_send.data(), packet_to_send.size());
              uint64_t* send_buf_as_64_array = (uint64_t*)send_buffer.get();
              send_buf_as_64_array[5] = fc_time_point_to_ntp_timestamp(fc::time_point::now()); // 5 = Transmit Timestamp
              // Guard: read_loop error recovery may have closed the socket
              // between the time this request_now was scheduled (via
              // ntp::request_now → async().wait()) and when it actually
              // executes on the ntp_thread.  Re-open if necessary so we
              // don't hit "send_to: Bad file descriptor".
              try {
                _sock.send_to(send_buffer, packet_to_send.size(), ep);
              } catch (const fc::exception&) {
                _sock.open();
                _sock.send_to(send_buffer, packet_to_send.size(), ep);
              }
              break;
            }
            ++i;
          }
          catch (const fc::canceled_exception&)
          {
            throw;
          }
          // this could fail to resolve but we want to go on to other hosts..
          catch ( const fc::exception& e )
          {
            std::string detail = e.to_detail_string();
            if( detail.find("Host not found") != std::string::npos ||
                detail.find("asio.netdb") != std::string::npos )
            {
              wlog( "\033[38;5;208mNTP server ${host}:${port} is unreachable (host not found), removing from list\033[0m",
                    ("host", item.first)("port", item.second) );
              _ntp_hosts.erase( _ntp_hosts.begin() + i );
            }
            else
            {
              elog( "${e}", ("e", detail ) );
              ++i;
            }
          }
          catch (...)
          {
            // Safety net: non-fc exceptions (e.g. boost::system::system_error
            // from a closed socket) must not escape to the async caller, as
            // that would crash the node when update_ntp_time()'s
            // async().wait() rethrows.
            ++i;
          }
        }
      } // request_now

      // started for first time in ntp() constructor, canceled in ~ntp() destructor
      // this task gets invoked every _retry_failed_request_interval_sec (currently 5 min), and if
      // _request_interval_sec (currently 1 hour) has passed since the last successful update,
      // it sends a new request
      void request_time_task()
      {
        assert(_ntp_thread.is_current());
        // Check if NTP hasn't been updated for too long
        if (_last_ntp_delta_initialized) {
          auto time_since_last_update = fc::time_point::now() - _last_valid_ntp_reply_received_time;
          if (time_since_last_update > fc::seconds(_request_interval_sec * 2)) {
            wlog("\033[94mNTP has not been updated for ${sec} seconds\033[0m", ("sec", time_since_last_update.count() / 1000000));
          }
        }
        if (_last_valid_ntp_reply_received_time <= fc::time_point::now() - fc::seconds(_request_interval_sec - 5))
          request_now();
        if (!_request_time_task_done.valid() || !_request_time_task_done.canceled())
          _request_time_task_done = schedule( [=](){ request_time_task(); },
                                              fc::time_point::now() + fc::seconds(_retry_failed_request_interval_sec),
                                              "request_time_task" );
      } // request_loop

      void start_read_loop()
      {
        _read_loop_done = _ntp_thread.async( [this](){ read_loop(); }, "ntp_read_loop" );
      }

      void read_loop()
      {
        assert(_ntp_thread.is_current());

        uint32_t receive_buffer_size = sizeof(uint64_t) * 1024;
        std::shared_ptr<char> receive_buffer(new char[receive_buffer_size], [](char* p){ delete[] p; });
        uint64_t* recv_buf = (uint64_t*)receive_buffer.get();

        //outer while to restart read-loop if exception is thrown while waiting to receive on socket.
        while( !_read_loop_done.canceled() )
        {
          // if you start the read while loop here, the recieve_from call will throw "invalid argument" on win32,
          // so instead we start the loop after making our first request
          try
          {
            _sock.open();
            request_time_task(); //this will re-send a time request

            while( !_read_loop_done.canceled() )
            {
              fc::ip::endpoint from;
              try
              {
                _sock.receive_from( receive_buffer, receive_buffer_size, from );
              //  wlog("received ntp reply from ${from}",("from",from) );
              } FC_RETHROW_EXCEPTIONS(error, "Error reading from NTP socket");

              fc::time_point receive_time = fc::time_point::now();
              fc::time_point origin_time = ntp_timestamp_to_fc_time_point(recv_buf[3]);
              fc::time_point server_receive_time = ntp_timestamp_to_fc_time_point(recv_buf[4]);
              fc::time_point server_transmit_time = ntp_timestamp_to_fc_time_point(recv_buf[5]);

              fc::microseconds offset(((server_receive_time - origin_time) +
                                       (server_transmit_time - receive_time)).count() / 2);
              fc::microseconds round_trip_delay((receive_time - origin_time) -
                                                (server_transmit_time - server_receive_time));
              //wlog("origin_time = ${origin_time}, server_receive_time = ${server_receive_time}, server_transmit_time = ${server_transmit_time}, receive_time = ${receive_time}",
              //     ("origin_time", origin_time)("server_receive_time", server_receive_time)("server_transmit_time", server_transmit_time)("receive_time", receive_time));
              // wlog("ntp offset: ${offset}, round_trip_delay ${delay}", ("offset", offset)("delay", round_trip_delay));

              //if the reply we just received has occurred more than the configured threshold after our last time request
              if( round_trip_delay > fc::microseconds(_round_trip_threshold_us) )
              {
                wlog("received stale ntp reply requested at ${request_time}, send a new time request", ("request_time", origin_time));
                request_now(); //request another reply and ignore this one
              }
              else //we think we have a timely reply, process it
              {
                if( offset < fc::seconds(60*60*24) && offset > fc::seconds(-60*60*24) )
                {
                  if( _valid_reply_received_this_cycle )
                  {
                    wlog("Ignoring additional NTP reply from ${endpoint} for this request cycle", ("endpoint", from));
                    continue;
                  }
                  int64_t new_delta = offset.count();

                  // Reject offsets that deviate too much from the moving average
                  bool should_accept = true;
                  if (_delta_history.size() >= 2) {
                    int64_t sum = 0;
                    for (int64_t d : _delta_history)
                      sum += d;
                    int64_t moving_avg = sum / static_cast<int64_t>(_delta_history.size());
                    int64_t deviation = std::abs(new_delta - moving_avg);
                    // Threshold: configured pct of |moving_avg|, with a configured minimum
                    int64_t threshold = std::max(std::abs(moving_avg) * static_cast<int64_t>(_rejection_threshold_pct) / 100, static_cast<int64_t>(_rejection_min_threshold_us));
                    if (deviation > threshold) {
                      wlog("\033[91mNTP delta rejected: ${new} us deviates ${dev} us from moving average ${avg} us (threshold ${thresh} us)\033[0m",
                           ("new", new_delta)("dev", deviation)("avg", moving_avg)("thresh", threshold));
                      should_accept = false;
                    }
                  }

                  if (should_accept) {
                    _valid_reply_received_this_cycle = true;
                    // Update moving average history
                    _delta_history.push_back(new_delta);
                    if (_delta_history.size() > _delta_history_max_size)
                      _delta_history.pop_front();

                    // Compute smoothed delta as moving average
                    int64_t sum = 0;
                    for (int64_t d : _delta_history)
                      sum += d;
                    int64_t smoothed_delta = sum / static_cast<int64_t>(_delta_history.size());

                    _last_ntp_delta_microseconds = smoothed_delta;
                    _last_ntp_delta_initialized = true;
                    fc::microseconds ntp_delta_time = fc::microseconds(_last_ntp_delta_microseconds);
                    _last_valid_ntp_reply_received_time = receive_time;
                    wlog("\033[94mntp_delta_time updated to ${delta_time} us (raw: ${raw} us, avg of ${count} samples)\033[0m",
                         ("delta_time", ntp_delta_time)("raw", new_delta)("count", _delta_history.size()));
                  }
                }
                else
                  elog( "NTP time and local time vary by more than a day! ntp:${ntp_time} local:${local}",
                       ("ntp_time", receive_time + offset)("local", fc::time_point::now()) );
              }
            }
          } // try
          catch (fc::canceled_exception)
          {
            throw;
          }
          catch (const fc::exception& e)
          {
            //swallow any other exception and restart loop
            elog("exception in read_loop, going to restart it. ${e}",("e",e));
          }
          catch (...)
          {
            //swallow any other exception and restart loop
            elog("unknown exception in read_loop, going to restart it.");
          }
          _sock.close();
          fc::usleep(fc::seconds(_retry_failed_request_interval_sec));
        } //outer while loop
        wlog("exiting ntp read_loop");
      } //end read_loop()
    }; //ntp_impl

  } // namespace detail



  ntp::ntp()
  :my( new detail::ntp_impl() )
  {
    my->start_read_loop();
  }

  ntp::~ntp()
  {
    my->_ntp_thread.async([=](){
      try
      {
        my->_request_time_task_done.cancel_and_wait("ntp object is destructing");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while shutting down NTP's request_time_task, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while shutting down NTP's request_time_task, ignoring" );
      }

      try
      {
        my->_read_loop_done.cancel_and_wait("ntp object is destructing");
      }
      catch ( const fc::exception& e )
      {
        wlog( "Exception thrown while shutting down NTP's read_loop, ignoring: ${e}", ("e",e) );
      }
      catch (...)
      {
        wlog( "Exception thrown while shutting down NTP's read_loop, ignoring" );
      }

    }, "ntp_shutdown_task").wait();
  }


  void ntp::add_server( const std::string& hostname, uint16_t port)
  {
    my->_ntp_thread.async( [=](){ my->_ntp_hosts.push_back( std::make_pair(hostname,port) ); }, "add_server" ).wait();
  }

  void ntp::set_request_interval( uint32_t interval_sec )
  {
    my->_request_interval_sec = interval_sec;
    my->_retry_failed_request_interval_sec = std::min(my->_retry_failed_request_interval_sec, interval_sec);
  }

  void ntp::set_retry_interval( uint32_t interval_sec )
  {
    my->_retry_failed_request_interval_sec = interval_sec;
  }

  void ntp::set_servers( const std::vector<std::pair<std::string, uint16_t>>& servers )
  {
    my->_ntp_thread.async( [this, servers](){
      my->_ntp_hosts.clear();
      for( const auto& s : servers )
        my->_ntp_hosts.push_back( s );
    }, "set_servers" ).wait();
  }

  void ntp::set_round_trip_threshold_ms( uint32_t ms )
  {
    my->_round_trip_threshold_us = ms * 1000;
  }

  void ntp::set_delta_history_size( size_t size )
  {
    my->_delta_history_max_size = size > 0 ? size : 1;
  }

  void ntp::set_rejection_threshold_pct( uint32_t pct )
  {
    my->_rejection_threshold_pct = pct;
  }

  void ntp::set_rejection_min_threshold_ms( uint32_t ms )
  {
    my->_rejection_min_threshold_us = ms * 1000;
  }

  void ntp::request_now()
  {
    my->_ntp_thread.async( [=](){ my->request_now(); }, "request_now" ).wait();
  }

  optional<time_point> ntp::get_time()const
  {
    if( my->_last_ntp_delta_initialized )
      return fc::time_point::now() + fc::microseconds(my->_last_ntp_delta_microseconds);
    return optional<time_point>();
  }

} //namespace fc
