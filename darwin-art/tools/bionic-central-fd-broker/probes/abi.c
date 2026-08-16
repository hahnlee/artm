#include "darwin_art_bionic_fd_broker.h"

_Static_assert(DARWIN_ART_FD_OWNER_ABI_V1 == 1, "owner ABI drift");
_Static_assert(DARWIN_ART_FD_FS_FILE == 1, "file kind drift");
_Static_assert(DARWIN_ART_FD_FS_RANDOM == 2, "random kind drift");
_Static_assert(DARWIN_ART_FD_STDIO == 3, "stdio kind drift");
_Static_assert(DARWIN_ART_FD_SOCKET == 4, "socket kind drift");
_Static_assert(sizeof(((DarwinArtFdPollEntry *)0)->fd) == sizeof(int),
               "guest descriptor width drift");

static DarwinArtFdBrokerStatus (*const publish_signature)(
    DarwinArtFdBroker *, DarwinArtFdOwnerHandle, uint64_t,
    int *) = darwin_art_fd_broker_publish;
static DarwinArtFdBrokerStatus (*const sendfile_signature)(
    DarwinArtFdBroker *, int, int, size_t,
    DarwinArtFdIoResult *) = darwin_art_fd_broker_sendfile;

int main(void) { return publish_signature == 0 || sendfile_signature == 0; }
