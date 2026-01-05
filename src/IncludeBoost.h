#pragma once

#ifdef U
#define TEMP_U
#undef U
#endif

//#define BOOST_URL_HEADER_ONLY 1
//#define BOOST_PROCESS_V2_HEADER_ONLY 1

#include <boost/pool/object_pool.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/asio.hpp>
#include <boost/optional.hpp>

#include <boost/process.hpp>
#include <boost/process/ext.hpp>  // Include this header

#include <boost/locale.hpp>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <boost/url.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace boost_bst = boost::beast;

namespace boost_opt = boost::program_options;
namespace boost_fs = boost::filesystem;
namespace boost_alg = boost::algorithm;
namespace boost_prc = boost::process;

#ifdef TEMP_U
#undef TEMP_U
#define U(x) _XPLATSTR(x)
#endif
