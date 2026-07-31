#ifndef HTTP_VERSION_HPP
#define HTTP_VERSION_HPP

#include <string>

// checks the request's HTTP version string. returns the status to send:
//   0   -> fine, it's HTTP/1.x
//   505 -> right shape but wrong major (HTTP/2.0, HTTP/9.9...)
//   400 -> not shaped like HTTP/<digit>.<digit> at all
int checkHttpVersion(const std::string& version);

#endif
