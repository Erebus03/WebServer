#include "../includes/Router.hpp"
#include "../includes/types.hpp"
#include <cassert>
#include <iostream>

int main () {
    ServerConfig server;

     LocationConfig loc1;
     loc1.path = "/";
     server.locations.push_back(loc1);

     LocationConfig loc2;
     loc2.path = "/uploads";
     server.locations.push_back(loc2);

    LocationConfig loc3;
    loc3.path = "/uploads/photos";
    server.locations.push_back(loc3);

     LocationConfig loc4;
     loc4.path = "/cgi-bin";
     server.locations.push_back(loc4);

     Router router;

     // Case 1: exact root match
     const LocationConfig *result = router.match("/", server);
     assert(result != NULL);
     assert(result->path == "/");

     // Case 2: longest prefix wins
     result = router.match("/uploads/photos/cat.jpg", server);
     assert(result != NULL);
     assert(result->path == "/uploads/photos");

     // Case 3: falls back to shorter location
     result = router.match("/uploads/document.pdf", server);
     assert(result != NULL);
     assert(result->path == "/uploads");

     // Case 4: falls back to root
     result = router.match("/anything-else", server);
     assert(result != NULL);
     assert(result->path == "/");

     // Case 5: CGI route
     result = router.match("/cgi-bin/test.py", server);
     assert(result != NULL);
     assert(result->path == "/cgi-bin");

     LocationConfig loc_trap;
     loc_trap.path = "/up";
     server.locations.push_back(loc_trap);

     result = router.match("/uploads/photo.jpg", server);
     assert(result != NULL);
     assert(result->path == "/uploads");   // must NOT be "/up"

     ServerConfig empty_server;
     LocationConfig only_uploads;
     only_uploads.path = "/uploads";
     empty_server.locations.push_back(only_uploads);

     result = router.match("/no-such-route", empty_server);
     assert(result == NULL);

     std::cout << "All router tests passed." << std::endl;

     return 0;
}
