#pragma once

#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <fc/time.hpp>
#include <fc/optional.hpp>


namespace fc {

    namespace detail {
        class ntp_impl;
    }

    class ntp {
    public:
        ntp();

        ~ntp();

        void add_server(const std::string &hostname, uint16_t port = 123);

        /** Replace the entire server list with the given one. */
        void set_servers(const std::vector<std::pair<std::string, uint16_t>>& servers);

        /** Set how often (in seconds) to request a time update (default: 900). */
        void set_request_interval(uint32_t interval_sec);

        /** Set the retry interval (in seconds) used when NTP has not replied (default: 300). */
        void set_retry_interval(uint32_t interval_sec);

        /** Set round-trip delay threshold in milliseconds; replies above this are discarded (default: 150). */
        void set_round_trip_threshold_ms(uint32_t ms);

        /** Set the moving-average history window size (default: 5). */
        void set_delta_history_size(size_t size);

        /** Set rejection threshold as a percentage of |moving_avg| (default: 50). */
        void set_rejection_threshold_pct(uint32_t pct);

        /** Set the minimum rejection threshold in milliseconds (default: 5). */
        void set_rejection_min_threshold_ms(uint32_t ms);

        void request_now();

        optional<time_point> get_time() const;

    private:
        std::unique_ptr<detail::ntp_impl> my;
    };

} // namespace fc
