// Copyright (c) 2014, Cloudera, inc.
#ifndef KUDU_UTIL_WEB_CALLBACK_REGISTRY_H
#define KUDU_UTIL_WEB_CALLBACK_REGISTRY_H

#include <boost/function.hpp>
#include <map>
#include <string>

namespace kudu {

// Interface for registering webserver callbacks.
class WebCallbackRegistry {
 public:
  typedef std::map<std::string, std::string> ArgumentMap;
  typedef boost::function<void (const ArgumentMap& args, std::stringstream* output)>
      PathHandlerCallback;

  virtual ~WebCallbackRegistry() {}

  // Register a callback for a URL path. Path should not include the
  // http://hostname/ prefix. If is_styled is true, the page is meant to be for
  // people to look at and is styled.  If false, it is meant to be for machines to
  // scrape.  If is_on_nav_bar is true,  a link to this page is
  // printed in the navigation bar at the top of each debug page. Otherwise the
  // link does not appear, and the page is rendered without HTML headers and
  // footers.
  // The first registration's choice of is_styled overrides all
  // subsequent registrations for that URL.
  virtual void RegisterPathHandler(const std::string& path, const PathHandlerCallback& callback,
                                   bool is_styled = true, bool is_on_nav_bar = true) = 0;
};

} // namespace kudu

#endif /* KUDU_UTIL_WEB_CALLBACK_REGISTRY_H */