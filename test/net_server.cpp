// Host build of the firmware's TCP session (NetProtocol + DivaState), fed
// with a simulated HORI NSW-230, for test_net.py.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <thread>
#include "../src/DivaState.h"
#include "../src/NetProtocol.h"
using namespace diva;
int main(int argc, char **argv) {
  int port = atoi(argv[1]);
  int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, (sockaddr *)&a, sizeof(a)) || listen(ls, 4)) return 1;
  std::thread([] {  // stdin: "plug", "unplug", or a HORI report in hex
    char line[256];
    const Profile *hori = findProfile(0x0F0D, 0x00FB);
    while (fgets(line, sizeof line, stdin)) {
      if (!strncmp(line, "plug", 4)) DivaState::setController(hori, 0x0F0D, 0x00FB);
      else if (!strncmp(line, "unplug", 6)) DivaState::setController(nullptr, 0, 0);
      else {
        uint8_t d[64]; size_t n = 0;
        for (char *p = line; p[0] && p[1] && p[0] != '\n'; p += 2) sscanf(p, "%2hhx", &d[n++]);
        uint8_t pkt[MAX_IN_PACKET]; uint16_t ext;
        size_t m = toDivaller(*hori, DivaState::options(), d, n, pkt, ext);
        if (m) DivaState::setInput(pkt, m, ext);
      }
      printf("ack native=%d\n", DivaState::nativeClients()); fflush(stdout);
    }
  }).detach();
  while (true) {
    int s = accept(ls, nullptr, nullptr);
    std::thread([s] { NetProtocol::serve(s, "test"); close(s); }).detach();
  }
}
