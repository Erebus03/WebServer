#include "../includes/PostHandler.hpp"
#include "../includes/HttpStatus.hpp"

HttpResponse PostHandler::handle(const HttpRequest& request, const LocationConfig& location)
{
    (void) request;
    (void) location;
    return HttpStatus::make_response(501);
}
