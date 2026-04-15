#include <fc/log/console_appender.hpp>
#include <fc/log/log_message.hpp>
#include <fc/thread/unique_lock.hpp>
#include <fc/string.hpp>
#include <fc/variant.hpp>
#include <fc/reflect/variant.hpp>
#ifndef WIN32
#include <unistd.h>
#endif
#include <boost/thread/mutex.hpp>
#define COLOR_CONSOLE 1
#include "console_defines.h"
#include <fc/io/stdio.hpp>
#include <fc/exception/exception.hpp>
#include <iomanip>
#include <sstream>


namespace fc {

   class console_appender::impl {
   public:
     config                      cfg;
     color::type                 lc[log_level::off+1];
#ifdef WIN32
     HANDLE                      console_handle;
#endif
   };

   console_appender::console_appender( const variant& args )
   :my(new impl)
   {
      configure( args.as<config>() );
   }

   console_appender::console_appender( const config& cfg )
   :my(new impl)
   {
      configure( cfg );
   }
   console_appender::console_appender()
   :my(new impl){}


   void console_appender::configure( const config& console_appender_config )
   { try {
#ifdef WIN32
      my->console_handle = INVALID_HANDLE_VALUE;
#endif
      my->cfg = console_appender_config;
#ifdef WIN32
         if (my->cfg.stream = stream::std_error)
           my->console_handle = GetStdHandle(STD_ERROR_HANDLE);
         else if (my->cfg.stream = stream::std_out)
           my->console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
#endif

         for( int i = 0; i < log_level::off+1; ++i )
            my->lc[i] = color::console_default;
         for( auto itr = my->cfg.level_colors.begin(); itr != my->cfg.level_colors.end(); ++itr )
            my->lc[itr->level] = itr->color;
   } FC_CAPTURE_AND_RETHROW( (console_appender_config) ) }

   console_appender::~console_appender() {}

   #ifdef WIN32
   static WORD
   #else
   static const char*
   #endif
   get_console_color(console_appender::color::type t ) {
      switch( t ) {
         case console_appender::color::red:          return CONSOLE_RED;
         case console_appender::color::green:        return CONSOLE_GREEN;
         case console_appender::color::brown:        return CONSOLE_BROWN;
         case console_appender::color::blue:         return CONSOLE_BLUE;
         case console_appender::color::magenta:      return CONSOLE_MAGENTA;
         case console_appender::color::cyan:         return CONSOLE_CYAN;
         case console_appender::color::white:        return CONSOLE_WHITE;
         case console_appender::color::light_gray:   return CONSOLE_LIGHT_GRAY;
         case console_appender::color::dark_gray:    return CONSOLE_DARK_GRAY;
         case console_appender::color::yellow:       return CONSOLE_YELLOW;
         case console_appender::color::bright_green: return CONSOLE_BRIGHT_GREEN;
         case console_appender::color::orange:       return CONSOLE_ORANGE;
         case console_appender::color::bright_blue:  return CONSOLE_BRIGHT_BLUE;
         case console_appender::color::console_default:
         default:
            return CONSOLE_DEFAULT;
      }
   }

   // Detect the appropriate console color for a log message based on its content.
   // Content patterns take priority over log-level colors, except that errors/warnings
   // not matching a specific pattern always get red.
   static console_appender::color::type detect_content_color(
      const std::string& message, log_level level)
   {
      // NTP — blue (overrides even warn/error level)
      if (message.find("NTP") != std::string::npos)
         return console_appender::color::bright_blue;

      // Errors and warnings — red
      if (level == log_level::error || level == log_level::warn)
         return console_appender::color::red;

      // Got block — yellow
      if (message.find("Got block") != std::string::npos)
         return console_appender::color::yellow;

      // Generated block — bright green
      if (message.find("Generated block") != std::string::npos)
         return console_appender::color::bright_green;

      // Snapshot export — bright green
      if (message.find("Creating snapshot") != std::string::npos ||
          message.find("Snapshot at block") != std::string::npos ||
          message.find("Exported ") != std::string::npos ||
          message.find("Compressed snapshot") != std::string::npos ||
          message.find("Snapshot created") != std::string::npos ||
          message.find("Periodic snapshot at block") != std::string::npos ||
          message.find("Reached snapshot-at-block") != std::string::npos ||
          message.find("Creating deferred snapshot") != std::string::npos ||
          message.find("Deferring snapshot") != std::string::npos ||
          message.find("Deferring periodic snapshot") != std::string::npos ||
          message.find("Snapshot cache updated") != std::string::npos ||
          message.find("Snapshot still deferred") != std::string::npos)
         return console_appender::color::bright_green;

      // Snapshot import — orange
      if (message.find("Loading snapshot from") != std::string::npos ||
          message.find("Snapshot loaded") != std::string::npos ||
          message.find("Decompressed snapshot") != std::string::npos ||
          message.find("from snapshot file") != std::string::npos ||
          message.find("Imported ") != std::string::npos ||
          message.find("Snapshot header validated") != std::string::npos ||
          message.find("Snapshot checksum") != std::string::npos ||
          message.find("Clearing genesis objects") != std::string::npos ||
          message.find("All objects imported") != std::string::npos ||
          message.find("Fork database seeded") != std::string::npos)
         return console_appender::color::orange;

      // Snapshot serving (server/client transfer) — yellow
      if (message.find("Snapshot server:") != std::string::npos ||
          message.find("Snapshot TCP server") != std::string::npos ||
          message.find("Download complete") != std::string::npos ||
          message.find("Downloaded ") != std::string::npos ||
          message.find("Querying snapshot") != std::string::npos ||
          message.find("Snapshot saved to") != std::string::npos ||
          message.find("Selected peer") != std::string::npos ||
          message.find("Peer ") != std::string::npos &&
              message.find("snapshot") != std::string::npos)
         return console_appender::color::yellow;

      // Elapsed time reply (JSON-RPC) — dark gray
      if (message.find("elapsed:") != std::string::npos)
         return console_appender::color::dark_gray;

      // Webserver API request (data: prefix from dump_rpc_time) — dark gray
      if (message.find("data: ") != std::string::npos)
         return console_appender::color::dark_gray;

      // Default — light gray
      return console_appender::color::light_gray;
   }

   boost::mutex& log_mutex() {
    static boost::mutex m; return m;
   }

   void console_appender::log( const log_message& m ) {
      //std::string message = fc::format_string( m.get_format(), m.get_data() );
      //fc::variant lmsg(m);

      FILE* out = stream::std_error ? stderr : stdout;

      //std::string fmt_str = fc::format_string( cfg.format, mutable_variant_object(m.get_context())( "message", message)  );
      std::stringstream file_line;
      file_line << m.get_context().get_file() << ":" << m.get_context().get_line_number() <<" ";
      ///////////////
      std::stringstream line;
      line << (m.get_context().get_timestamp().time_since_epoch().count() % (1000ll*1000ll*60ll*60))/1000 <<"ms ";

      line << std::setw( 10 ) << std::left << m.get_context().get_thread_name().substr(0,9).c_str() <<" "<<std::setw(30)<< std::left <<file_line.str();

      auto me = m.get_context().get_method();
      // strip all leading scopes...
      if( me.size() )
      {
         uint32_t p = 0;
         for( uint32_t i = 0;i < me.size(); ++i )
         {
             if( me[i] == ':' ) p = i;
         }

         if( me[p] == ':' ) ++p;
         line << std::setw( 20 ) << std::left << m.get_context().get_method().substr(p,20).c_str() <<" ";
      }
      line << "] ";

      std::string message = fc::format_string( m.get_format(), m.get_data() );
      line << message;//.c_str();

      fc::unique_lock<boost::mutex> lock(log_mutex());

      print( line.str(), detect_content_color(message, m.get_context().get_log_level()) );

      fprintf( out, "\n" );

      if( my->cfg.flush ) fflush( out );
   }

   void console_appender::print( const std::string& text, color::type text_color )
   {
      FILE* out = stream::std_error ? stderr : stdout;

      #ifdef WIN32
         if (my->console_handle != INVALID_HANDLE_VALUE)
           SetConsoleTextAttribute(my->console_handle, get_console_color(text_color));
      #else
         if(isatty(fileno(out))) fprintf( out, "\r%s", get_console_color( text_color ) );
      #endif

      if( text.size() )
         fprintf( out, "%s", text.c_str() ); //fmt_str.c_str() );

      #ifdef WIN32
      if (my->console_handle != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(my->console_handle, CONSOLE_DEFAULT);
      #else
      if(isatty(fileno(out))) fprintf( out, "\r%s", CONSOLE_DEFAULT );
      #endif

      if( my->cfg.flush ) fflush( out );
   }

   fc::console_appender::color::type console_appender::get_text_color( const log_message& m ) const {
      return my->lc[m.get_context().get_log_level()];
   }
   bool console_appender::can_flush() const {
      return my->cfg.flush;
   }
}
