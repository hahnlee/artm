#ifndef DARWIN_ART_PROVIDER_OWNERS_H_
#define DARWIN_ART_PROVIDER_OWNERS_H_

#include <string>

namespace darwin_art::providers {

bool acquire_filesystem(int directory_fd, std::string* error);
void release_filesystem();

bool acquire_network(std::string* error);
void release_network();

bool acquire_stdio(std::string* error);
void release_stdio();

bool acquire_ioctl(std::string* error);
void release_ioctl();

bool acquire_strftime(std::string* error);
void release_strftime();

bool acquire_sendfile(std::string* error);
void release_sendfile();

}  // namespace darwin_art::providers

#endif  // DARWIN_ART_PROVIDER_OWNERS_H_
