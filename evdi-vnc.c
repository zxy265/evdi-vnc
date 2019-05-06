#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include <regex.h>

#include <evdi_lib.h>
#include <rfb/rfb.h>

// *** Constants ***

// Hardcode an EDID from the Google Autotest project:
// https://chromium.googlesource.com/chromiumos/third_party/autotest/+/master/server/site_tests/display_Resolution/test_data/edids
// Dumped with xxd --include
static const unsigned char EDID[] = {
  0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4e, 0x8d, 0x54, 0x24,
  0x00, 0x00, 0x00, 0x00, 0x08, 0x16, 0x01, 0x04, 0xa5, 0x66, 0x39, 0x78,
  0x22, 0x0d, 0xc9, 0xa0, 0x57, 0x47, 0x98, 0x27, 0x12, 0x48, 0x4c, 0xbf,
  0xef, 0x80, 0x81, 0x80, 0x81, 0x40, 0x71, 0x4f, 0x81, 0x00, 0xb3, 0x00,
  0x95, 0x00, 0x95, 0x0f, 0xa9, 0x40, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38,
  0x2d, 0x40, 0x58, 0x2c, 0x45, 0x00, 0xfa, 0x3c, 0x32, 0x00, 0x00, 0x1e,
  0x66, 0x21, 0x50, 0xb0, 0x51, 0x00, 0x1b, 0x30, 0x40, 0x70, 0x36, 0x00,
  0xfa, 0x3c, 0x32, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x38,
  0x4b, 0x1e, 0x51, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
  0x00, 0x00, 0x00, 0xfc, 0x00, 0x41, 0x74, 0x68, 0x65, 0x6e, 0x61, 0x44,
  0x50, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x01, 0x35, 0x02, 0x03, 0x12, 0xc1,
  0x45, 0x90, 0x1f, 0x04, 0x13, 0x03, 0x23, 0x09, 0x07, 0x07, 0x83, 0x01,
  0x00, 0x00, 0x02, 0x3a, 0x80, 0xd0, 0x72, 0x38, 0x2d, 0x40, 0x10, 0x2c,
  0x45, 0x80, 0xfa, 0x3c, 0x32, 0x00, 0x00, 0x1e, 0x01, 0x1d, 0x00, 0x72,
  0x51, 0xd0, 0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00, 0xfa, 0x3c, 0x32, 0x00,
  0x00, 0x1e, 0x01, 0x1d, 0x00, 0xbc, 0x52, 0xd0, 0x1e, 0x20, 0xb8, 0x28,
  0x55, 0x40, 0xfa, 0x3c, 0x32, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x9d
};
// Currently the EVDI library won't update more than 16 rects at a time
#define MAX_RECTS 16

// *** Globals ***

int connectedClients = 0;
rfbScreenInfoPtr screen;

evdi_handle evdiNode;
bool bufferAllocated = false;
evdi_buffer buffer;
evdi_mode currentMode;
evdi_rect rects[MAX_RECTS];

// *** Signal Handler ***
void handleSignal(int signal) {
  if (signal == SIGINT) {
    fprintf(stdout, "Shutting down VNC server.\n");
    rfbShutdownServer(screen, true);
  }
}

// *** VNC Hooks ***
static void clientGone(rfbClientPtr cl)
{
  cl->clientData = NULL;
  connectedClients--;
}

static enum rfbNewClientAction newClient(rfbClientPtr client)
{
  client->clientGoneHook = clientGone;
  connectedClients++;
  return RFB_CLIENT_ACCEPT;
}


// *** Other VNC functions ***

/* Adjust the pixel format to match the EVDI buffer.
 */
void adjustPixelFormat(rfbScreenInfoPtr screen) {
  // TODO: properly communicate between Linux DRM buffer formats and
  // libvncserver
  rfbPixelFormat *format = &screen->serverFormat;
  format->redShift = 16;
  format->blueShift = 0;
  fprintf(stdout, "Pixel format adjusted.\n");
}

/* Register a VNC frame buffer sized for the current mode
 */
char * allocateVncFramebuffer(rfbScreenInfoPtr screen) {
  // Use the EVDI buffer
  return buffer.buffer;
}

/* Do initial VNC setup and start the server. 
 * Returns the rfbScreenInfoPtr created.
 */
rfbScreenInfoPtr startVncServer(int argc, char *argv[]) {
  rfbScreenInfoPtr screen = rfbGetScreen(&argc, argv, currentMode.width,
      currentMode.height, 8, 3, currentMode.bits_per_pixel/8);
  if (screen == 0) {
    fprintf(stderr, "Error getting RFB screen.\n");
    return screen;
  }
  printf("!!!!!!@#@!\n");
  adjustPixelFormat(screen);
  rfbPixelFormat *format = &screen->serverFormat;
  fprintf(stdout,
      "Pixel format:\nShift:\tR: %d G: %d B: %d\nMax:\tR: %d G: %d B: %d\n",
      format->redShift, format->greenShift, format->blueShift,
      format->redMax, format->greenMax, format->blueMax);
  screen->newClientHook = newClient;
  screen->frameBuffer = allocateVncFramebuffer(screen);
  rfbInitServer(screen);
  return screen;
}

void cleanUpVncServer(rfbScreenInfoPtr screen) {
  free(screen->frameBuffer);
  rfbScreenCleanup(screen);
}

// *** EVDI Hooks ***

void dpmsHandler(int dpmsMode, void *userData) {
  fprintf(stdout, "TODO: Handle DPMS mode changes\n");
}

void modeChangedHandler(evdi_mode mode, void *userData) {
  fprintf(stdout, "Mode changed to %dx%d @ %dHz\n", mode.width, mode.height, mode.refresh_rate);
  if (mode.bits_per_pixel != 32) {
    // TODO: properly communicate between Linux DRM buffer formats and
    // libvncserver
    fprintf(stderr, "evdi-vnc requires modes with 32 bits per pixel. Instead received %d bpp.",
        mode.bits_per_pixel);
    exit(1);
  }
  currentMode = mode;

  // Unregister old EVDI buffer if necessary
  if (bufferAllocated) {
    free(buffer.buffer);
    evdi_unregister_buffer(evdiNode, buffer.id);
  }
  // Register new EVDI buffer for this mode
  buffer.id = 0;
  buffer.width = mode.width;
  buffer.height = mode.height;
  buffer.stride = mode.bits_per_pixel/8 * mode.width;
  buffer.buffer = malloc(buffer.height * buffer.stride);
  evdi_register_buffer(evdiNode, buffer);
  bufferAllocated = true;

  // Register new VNC framebuffer
  if (screen == 0) return; // Exit early if VNC hasn't been started yet
  char *newFb = allocateVncFramebuffer(screen);
  rfbNewFramebuffer(screen, newFb, currentMode.width, currentMode.height, 8, 3,
      currentMode.bits_per_pixel/8);
  // Update pixel format
  adjustPixelFormat(screen);
}

void updateReadyHandler(int bufferId, void *userData) {
  int nRects;
  evdi_grab_pixels(evdiNode, rects, &nRects);

  for (int i = 0; i < nRects; i++) {
    for (int y = rects[i].y1; y <= rects[i].y2; y++) {
      rfbMarkRectAsModified(screen, rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2);
    }
  }
}

void crtcStateHandler(int state, void *userData) {
  fprintf(stdout, "TODO: Handle CRTC state changes\n");
}

// *** Other EVDI functions ***

/* Count the number of cardX files in /sys/class/drm. */
int countCardEntries() {
  // Prepare regex to match cardX.
  regex_t regex;
  if (regcomp(&regex, "^card[0-9]*$", REG_NOSUB)) {
    fprintf(stderr, "Could not compile card regex.\n");
    return 0;
  }

  // Scan through /sys/class/drm
  int entries = 0;
  DIR *dir = opendir("/sys/class/drm");
  if (dir != NULL) {
    struct dirent *dirEntry;
    while ((dirEntry = readdir(dir))) {
      if (regexec(&regex, dirEntry->d_name, 0, NULL, 0) == 0) {
        entries++;
      }
    }
  } else {
    fprintf(stderr, "Could not open /sys/class/drm\n");
  }
  return entries;
}
/* Scan through potential EVDI devices until either one is available or we've probed all devices.
 * Return the index of the first available device or -1 if none is found.
 */
int findAvailableEvdiNode() {
  evdi_device_status status = UNRECOGNIZED;
  int i;
  int nCards = countCardEntries();
  for (i = 0; i < nCards; i++) {
    status = evdi_check_device(i);
    if (status == AVAILABLE) {
      return i;
    }
  }
  return -1;
}

/* Search and open the first available EVDI node.
 * If none already exists create one and open it.
 * Returns an evdi_handle if successful and EVDI_INVALID_HANDLE if not.
 */
evdi_handle openEvdiNode() {
  // First, find an available node
  int nodeIndex = findAvailableEvdiNode();
  if (nodeIndex == -1) {
    // Create a new node instead
    if (evdi_add_device() == 0) {
      fprintf(stderr, "Failed to create a new EVDI node.\n");
      return EVDI_INVALID_HANDLE;
    }
    nodeIndex = findAvailableEvdiNode();
    if (nodeIndex == -1) {
      fprintf(stderr, "Failed to find newly created EVDI node.\n");
      return EVDI_INVALID_HANDLE;
    }
  }

  // Next, open the node we found
  evdi_handle nodeHandle = evdi_open(nodeIndex);
  if (nodeHandle == EVDI_INVALID_HANDLE) {
    fprintf(stderr, "Failed to open EVDI node: %d\n", nodeIndex);
    return EVDI_INVALID_HANDLE;
  }
  return nodeHandle;
}

/* Connect to the given EVDI node.
 */
void connectToEvdiNode(evdi_handle nodeHandle) {
  fprintf(stdout, "Sent EDID of size: %lu\t%s\n", sizeof(EDID), EDID);
  evdi_connect(nodeHandle, EDID, sizeof(EDID));
  fprintf(stdout, "Connected to EVDI node.\n");
}

void disconnectFromEvdiNode(evdi_handle nodeHandle) {
  evdi_disconnect(nodeHandle);
  fprintf(stdout, "Disconnected from EVDI node.\n");
}

// *** Main ***

int main(int argc, char *argv[]) {

  // Setup EVDI
  evdiNode = openEvdiNode();
  if (evdiNode == EVDI_INVALID_HANDLE) {
    fprintf(stderr, "Failed to connect to an EVDI node.\n");
    return 1;
  }
  evdi_selectable evdiFd = evdi_get_event_ready(evdiNode);
  struct pollfd pollfds[1];
  pollfds[0].fd = evdiFd;
  pollfds[0].events = POLLIN;
  evdi_event_context evdiCtx;
  evdiCtx.dpms_handler = dpmsHandler;
  evdiCtx.mode_changed_handler = modeChangedHandler;
  evdiCtx.update_ready_handler = updateReadyHandler;
  evdiCtx.crtc_state_handler = crtcStateHandler;
  
  // Connect to evdiNode and wait for mode to update
  connectToEvdiNode(evdiNode);
  while (currentMode.width == 0) {
    if (poll(pollfds, 1, -1)) {
      // Figure out which update we received
      evdi_handle_events(evdiNode, &evdiCtx);
    }
  }
  printf("!!!!!!!!!Try to Start vnc server!\n");
  // Start up VNC server
  screen = startVncServer(argc, argv);
  if (screen == 0) {
    fprintf(stderr, "Failed to start VNC server.\n");
    return 1;
  }

  // Catch Ctrl-C (SIGINT)
  struct sigaction sa;
  sa.sa_handler = handleSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);

  // Run event loop
  fprintf(stdout, "Starting event loop.\n");
  while (rfbIsActive(screen)) {
    // Request updates
    while (evdi_request_update(evdiNode, buffer.id)) {
      updateReadyHandler(buffer.id, NULL);
    }
    // Poll for EVDI updates for 1.0ms
    if (poll(pollfds, 1, 1)) {
      // Figure out which update we received
      evdi_handle_events(evdiNode, &evdiCtx);
    }
    // Check for VNC events for remaining time to match refresh rate
    int timeoutMicros = (1e6 / currentMode.refresh_rate) - 1000;
    rfbProcessEvents(screen, timeoutMicros);
  }

  // Clean up
  fprintf(stdout, "Cleaning up...\n");
  cleanUpVncServer(screen);
  disconnectFromEvdiNode(evdiNode);
  evdi_close(evdiNode);

  return 0;
}
