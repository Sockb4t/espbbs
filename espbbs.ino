#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <stdarg.h>

#include "FS.h"

WiFiServer server(23);

#define MAX_INPUT 255
#define MAX_PATH 255

// Number of bytes that we buffer at a time while streaming files to clients
#define FILE_STREAM_BUFFER_SIZE 64

// Max number of lines to send client at once, when paging textfiles
#define LINES_PER_PAGE        25

// This pin will be set to high when a client connects
#define CONNECT_INDICATOR_PIN D1
// Duration in millis for how long we will leave the connect indicator pin high
#define CONNECT_INDICATOR_DURATION 3000

#define STAGE_INIT            0

// Login
#define BBS_LOGIN             1
#define BBS_LOGIN_USERNAME    1
#define BBS_LOGIN_PASSWORD    2

// Guest
#define BBS_GUEST             2

// Main Menu
#define BBS_MAIN              3
#define BBS_MAIN_SELECT       1

// Logout
#define BBS_LOGOUT            4

// File Area
#define BBS_FILES             5
#define BBS_FILES_LIST        1
#define BBS_FILES_READ        2

// Users
#define BBS_USER              6
#define BBS_USER_NEW_NAME     1
#define BBS_USER_NEW_PASSWORD 2
#define BBS_USER_NEW_CONFIRM  3
#define BBS_USER_NEW_FINALIZE 4
#define BBS_USER_WHOS_ONLINE  10

// MTNC
#define BBS_MTNC              7
#define BBS_MTNC_READ         1
#define BBS_MTNC_SET          2

#define BBS_PAUSE             8

#define BBS_NCHAT             9
#define BBS_NCHAT_SEND        1
#define BBS_NCHAT_SELECT      2

char data[1500];
int ind = 0;

struct BBSUser {
  char username[64]; // alphanumeric, lowercase
  char password[64]; // Plaintext - not too concerned about actual security here, folks!
  char twitterHandle[64];
  char githubHandle[64];
};

struct BBSClient {
  int action;
  int stage;
  int nextAction;
  int nextStage;
  bool inputting;
  char input[MAX_INPUT];
  int inputPos;
  char inputEcho;
  bool inputSingle;
  void *data;
  int chatbuddy;
  BBSUser user;
};

struct BBSFileClient {
  char path[MAX_PATH];
  File f;
  char buffer[FILE_STREAM_BUFFER_SIZE];
  int bufferPosition;
  int bufferUsed;
  int lineCount;
  bool nonstop;
};

#define MTNC_MAX_LENGTH 140

struct BBSInfo {
  int callersTotal;
  int callersToday;
  char mtnc[MTNC_MAX_LENGTH];
};

#define MAX_CLIENTS 6

WiFiClient clients[MAX_CLIENTS];
BBSClient bbsclients[MAX_CLIENTS];
BBSInfo bbsInfo;
long connectIndicatorMillis;

void persistBBSInfo() {
  File f = SPIFFS.open("/bbsinfo.dat", "w+");
  if (f) {
    f.write((unsigned char *)&bbsInfo, sizeof(bbsInfo));
    f.close();
    Serial.println("SPIFFS bbsinfo written.");
  }
}

void setup() {
  Serial.begin(115200);

  SPIFFS.begin();

  File f = SPIFFS.open("/bbsinfo.dat", "r");
  if (f) {
    f.readBytes((char *)&bbsInfo, sizeof(bbsInfo));
    f.close();
    Serial.println("SPIFFS bbsinfo read.");
  }

  pinMode(CONNECT_INDICATOR_PIN, OUTPUT);

  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  wifiManager.autoConnect();

  Serial.println("Connection established.");

  server.begin();
  Serial.println("Server started: ");
  Serial.println(WiFi.localIP());
}

void sendTextFile(WiFiClient client, String filepath) {
  File f = SPIFFS.open(filepath, "r");
  if (!f) {
    client.write("File open failed\r\n");
    Serial.println("SPIFFS file open failed");
  } else {
    size_t size = f.size();
    if ( size > 0 ) {
      char buf[32];
      char buf2[32];
      int len = 1;
      int ofs = 0;

      while (len) {
        ofs = 0;
        len = f.readBytes(buf, 32);
        for (int i = 0; i < len; i++) {
          if (buf[i] == 10 || i + 1 == len) {
            buf2[ofs++] = buf[i];
            if (buf[i] == 10) {
              buf2[ofs++] = '\r';
            }
            client.write((uint8_t *)buf2, ofs);
            ofs = 0;
          } else {
            buf2[ofs++] = buf[i];
          }
        }
      }

    }
    f.close();
  }
}

void clearEntireLine(int clientNumber) {
  cprintf(clientNumber, "\33[2K\r");
}

void pageTextFile(int clientNumber) {

  char buf2[FILE_STREAM_BUFFER_SIZE];
  int ofs = 0;
  BBSFileClient *client = ((BBSFileClient *)(bbsclients[clientNumber].data));

  // If we have processed all of our buffered data, read in some more
  if (client->bufferPosition >= client->bufferUsed) {
    client->bufferPosition = 0;
    client->bufferUsed = client->f.readBytes(client->buffer, FILE_STREAM_BUFFER_SIZE);
  }

  for (int i = client->bufferPosition; i < client->bufferUsed; i++) {
    if (client->buffer[i] == 10) {
      buf2[ofs++] = client->buffer[i];
      if (client->buffer[i] == 10) {
        buf2[ofs++] = '\r';
        client->lineCount++;
      }
      clients[clientNumber].write((uint8_t *)buf2, ofs);
      ofs = 0;

      if (client->lineCount >= LINES_PER_PAGE) {
        if (!client->nonstop) {
          cprintf(clientNumber, "ESC=Cancel, Space=Continue, Enter=Nonstop");
          getInputSingle(clientNumber);
          client->lineCount = 0;
        }
        client->bufferPosition = i + 1;
        return;
      }
    } else {
      buf2[ofs++] = client->buffer[i];
    }
  }
  // write what we've buffered to the client
  if (ofs > 0) {
    clients[clientNumber].write((uint8_t *)buf2, ofs);
  }

  // Record final buffer position
  client->bufferPosition = client->bufferUsed;

  // If we did not obtain any more data this pass, close file
  if (!client->bufferUsed) {
    client->f.close();
  }
}

void cprintf(int clientNumber, const char *fmt, ...) {
  char buf[255];
  va_list va;
  va_start (va, fmt);
  vsnprintf (buf, sizeof(buf), fmt, va);
  va_end (va);

  if (clients[clientNumber]) clients[clientNumber].write((uint8_t *)buf, strlen(buf));
}

void getInput(int clientNumber) {
  discardInput(clientNumber);
  bbsclients[clientNumber].inputEcho = 0;
  bbsclients[clientNumber].input[0] = 0;
  bbsclients[clientNumber].inputSingle = false;
  bbsclients[clientNumber].inputting = true;
}

void getInput(int clientNumber, char echo) {
  getInput(clientNumber);
  bbsclients[clientNumber].inputEcho = echo;
}

void getInputSingle(int clientNumber) {
  getInput(clientNumber);
  bbsclients[clientNumber].inputSingle = true;
}

void discardInput(int clientNumber) {
  bbsclients[clientNumber].input[0] = 0;
  bbsclients[clientNumber].inputPos = 0;

  clients[clientNumber].flush();
}

bool hasInput(int clientNumber) {
  return strlen(bbsclients[clientNumber].input) > 0;
}

void action (int clientNumber, int actionId, int stageId) {
  bbsclients[clientNumber].action = actionId;
  bbsclients[clientNumber].stage = stageId;

  // reset input when we change actions
  bbsclients[clientNumber].input[0] = 0;
}

void action (int clientNumber, int actionId) {
  discardInput(clientNumber);
  action(clientNumber, actionId, STAGE_INIT);
}

void actionWithPause(int clientNumber, int actionId, int stageId) {
  bbsclients[clientNumber].nextAction = actionId;
  bbsclients[clientNumber].nextStage = stageId;

  action(clientNumber, BBS_PAUSE);
}

void actionWithPause(int clientNumber, int actionId) {
  actionWithPause(clientNumber, actionId, STAGE_INIT);
}

void handleBBSUser(int clientNumber) {
  File f; // Declared here since compiler complains about case crossing otherwise. :P

  switch (bbsclients[clientNumber].stage) {
    case STAGE_INIT:
      action(clientNumber, BBS_USER, BBS_USER_NEW_NAME);
      break;
    case BBS_USER_NEW_NAME:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input) > 0) {
          bool valid = true;
          for (int j = 0; j < strlen(bbsclients[clientNumber].input) && valid; j++) {
            if (!isalnum(bbsclients[clientNumber].input[j])) valid = false;
            else bbsclients[clientNumber].input[j] = tolower(bbsclients[clientNumber].input[j]);
          }
          if (!valid) {
            cprintf(clientNumber, "Non-alphanumeric character detected.\r\n");
          } else {
            char testPath[255];
            sprintf(testPath, "/users/%s.dat", bbsclients[clientNumber].input);
            if (SPIFFS.exists(testPath)) {
              cprintf(clientNumber, "Username '%s' is already taken.\r\n", bbsclients[clientNumber].input);
              valid = false;
            } else {
              if (strcmp(bbsclients[clientNumber].input, "new") == 0 ||
                  strcmp(bbsclients[clientNumber].input, "guest") == 0 ||
                  strcmp(bbsclients[clientNumber].input, "sysop") == 0) {
                cprintf(clientNumber, "Username '%s' is not allowed.\r\n", bbsclients[clientNumber].input);
                valid = false;
              }
            }
          }

          if (valid) {
            strcpy(bbsclients[clientNumber].user.username, bbsclients[clientNumber].input);
            action(clientNumber, BBS_USER, BBS_USER_NEW_PASSWORD);
          } else {
            discardInput(clientNumber);
          }
        } else {
          cprintf(clientNumber, "Enter your desired username (lowercase, alphanumeric only): ");
          getInput(clientNumber);
        }
      }
      break;
    case BBS_USER_NEW_PASSWORD:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input) > 0) {
          strcpy(bbsclients[clientNumber].user.password, bbsclients[clientNumber].input);
          action(clientNumber, BBS_USER, BBS_USER_NEW_CONFIRM);
        } else {
          cprintf(clientNumber, "Enter password (64 character maximum): ");
          getInput(clientNumber, '*');
        }
      }
      break;
    case BBS_USER_NEW_CONFIRM:
      if (!bbsclients[clientNumber].inputting) {
        if (strlen(bbsclients[clientNumber].input) > 0) {
          if (strcmp(bbsclients[clientNumber].user.password, bbsclients[clientNumber].input) != 0) {
            cprintf(clientNumber, "Passwords do not match.\r\n");
            action(clientNumber, BBS_USER, BBS_USER_NEW_PASSWORD);
          } else {
            action(clientNumber, BBS_USER, BBS_USER_NEW_FINALIZE);
          }
        } else {
          cprintf(clientNumber, "Re-enter password (64 character maximum): ");
          getInput(clientNumber, '*');
        }
      }
      break;
    case BBS_USER_NEW_FINALIZE:
      char userPath[255];
      sprintf(userPath, "/users/%s.dat", bbsclients[clientNumber].user.username);
      f = SPIFFS.open(userPath, "w+");
      if (f) {
        f.write((unsigned char *) & (bbsclients[clientNumber].user), sizeof(bbsclients[clientNumber].user));
        f.close();
        action(clientNumber, BBS_MAIN);
      } else {
        cprintf(clientNumber, "Fatal Error - Unable to create user file.");
        action(clientNumber, BBS_LOGIN);
      }
      break;
    case BBS_USER_WHOS_ONLINE:
      for (int j = 0; j < MAX_CLIENTS; j++) {
        if (clients[j]) {
          cprintf(clientNumber, "Node #%u: %s\r\n", j, bbsclients[j].user.username);
        } else {
          cprintf(clientNumber, "Node #%u: Open\r\n", j);
        }
        action(clientNumber, BBS_MAIN);
      }
      break;
  }
}

void loop() {
  char buf[128];
  char charIn;
  char buffer[16];
  char result;

  // file stuff
  int pathTokCount = 0;
  int fileTokCount = 0;
  char tokBuffer[MAX_PATH];
  char lastPathComponent[MAX_PATH];
  char currentPathComponent[MAX_PATH];
  char *token;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i]) {
      if (clients[i].status() == CLOSED) {
        clients[i].stop();
      } else {
        switch (bbsclients[i].action) {
          case BBS_LOGIN:
            switch (bbsclients[i].stage) {
              case STAGE_INIT:
                action(i, BBS_LOGIN, BBS_LOGIN_USERNAME);
                break;
              case BBS_LOGIN_USERNAME:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    if (strcmp(bbsclients[i].input, "guest") == 0) {
                      strcpy(bbsclients[i].user.username, "guest");
                      action(i, BBS_GUEST);
                    } else if (strcmp(bbsclients[i].input, "new") == 0) {
                      strcpy(bbsclients[i].user.username, "new user");
                      action(i, BBS_USER);
                    } else {
                      bool valid = true;

                      for (int j = 0; j < strlen(bbsclients[i].input) && valid; j++) {
                        if (!isalnum(bbsclients[i].input[j])) valid = false;
                        else bbsclients[i].input[j] = tolower(bbsclients[i].input[j]);
                      }

                      if (valid) {
                        strcpy(bbsclients[i].user.username, bbsclients[i].input);

                        action(i, BBS_LOGIN, BBS_LOGIN_PASSWORD);
                      } else {
                        cprintf(i, "Invalid username. Usernames are lowercase, alphanumeric only.\r\n");
                      }
                    }

                    // clear input
                    discardInput(i);
                  } else {
                    cprintf(i, "Node #%u - Username: ", i);
                    getInput(i);
                  }
                }
                break;
              case BBS_LOGIN_PASSWORD:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    bool valid = false;
                    char testPath[255];
                    sprintf(testPath, "/users/%s.dat", bbsclients[i].user.username);
                    if (SPIFFS.exists(testPath)) {
                      File f = SPIFFS.open(testPath, "r");
                      if (f) {
                        f.readBytes((char *)&bbsclients[i].user, sizeof(bbsclients[i].user));
                        f.close();
                        if (strcmp(bbsclients[i].user.password, bbsclients[i].input) == 0) valid = true;
                      }
                    }
                    if (!valid) {
                      cprintf(i, "Username or password incorrect.\r\n");
                    }
                    action(i, valid ? BBS_MTNC : BBS_LOGIN);
                  } else {
                    cprintf(i, "Password: ");
                    getInput(i, '*');
                  }
                }
                break;
            }
            break;
          case BBS_USER:
            handleBBSUser(i);
            break;
          case BBS_GUEST:
            switch (bbsclients[i].stage) {
              case STAGE_INIT:
                cprintf(i, "Welcome, guest!\r\n");
                actionWithPause(i, BBS_MTNC);
                break;
            }
            break;
          case BBS_FILES:
            switch (bbsclients[i].stage) {
              case STAGE_INIT: {
                  Dir dir = SPIFFS.openDir(((BBSFileClient *)(bbsclients[i].data))->path);
                  int idx = 1;

                  lastPathComponent[0] = 0;
                  currentPathComponent[0] = 0;
                  pathTokCount = 0;
                  strcpy(tokBuffer, ((BBSFileClient *)(bbsclients[i].data))->path);
                  token = strtok(tokBuffer, "/");
                  while (token != NULL) {
                    pathTokCount++;
                    token = strtok(NULL, "/");
                  }

                  cprintf(i, "Browsing %s\r\n", ((BBSFileClient *)(bbsclients[i].data))->path);
                  cprintf(i, "X) Exit File Library\r\n");
                  if (pathTokCount > 1) cprintf(i, "B) Go Back\r\n");

                  while (dir.next()) {
                    if (strncmp(dir.fileName().c_str(), ((BBSFileClient *)(bbsclients[i].data))->path, strlen(((BBSFileClient *)(bbsclients[i].data))->path)) == 0) {
                      fileTokCount = 0;
                      strcpy(tokBuffer, (char *)dir.fileName().c_str());
                      token = strtok(tokBuffer, "/");
                      while (token != NULL) {
                        fileTokCount++;
                        token = strtok(NULL, "/");
                        if (fileTokCount == pathTokCount) {
                          strcpy(currentPathComponent, token);
                        }
                      }

                      if (fileTokCount == pathTokCount + 1) { // a file within our current path
                        cprintf(i, "%u) %s\r\n", idx++, dir.fileName().c_str());
                      } else if (fileTokCount > pathTokCount + 1) { // path to a file deeper than our current path
                        if (strcmp(lastPathComponent, currentPathComponent) != 0) {
                          cprintf(i, "%u) %s/\r\n", idx++, currentPathComponent);
                          strcpy(lastPathComponent, currentPathComponent);
                        }
                      }
                    }
                  }
                  getInput(i);

                  action(i, BBS_FILES, BBS_FILES_LIST);
                } break;

              case BBS_FILES_LIST: {
                  if (!bbsclients[i].inputting) {
                    if (toupper(bbsclients[i].input[0]) == 'B') { // Back
                      strcpy(tokBuffer, ((BBSFileClient *)(bbsclients[i].data))->path);
                      token = strtok(tokBuffer, "/");

                      while (token != NULL) {
                        pathTokCount++;
                        token = strtok(NULL, "/");
                      }

                      if (pathTokCount == 1) {
                        cprintf(i, "You are already at the root directory of this File Library.\r\n");
                        action(i, BBS_FILES);
                      } else { // Tokenize the current path, clear path, and copy tokens back in except for last one
                        strcpy(tokBuffer, ((BBSFileClient *)(bbsclients[i].data))->path);
                        token = strtok(tokBuffer, "/");

                        ((BBSFileClient *)(bbsclients[i].data))->path[0] = 0;

                        while (token != NULL && pathTokCount > 1) {
                          sprintf(((BBSFileClient *)(bbsclients[i].data))->path, "%s/%s", ((BBSFileClient *)(bbsclients[i].data))->path, token);
                          pathTokCount--;
                          token = strtok(NULL, "/");
                        }
                        action(i, BBS_FILES);
                      }
                    } else if (toupper(bbsclients[i].input[0]) == 'X') { // Exit File Library
                      action(i, BBS_MAIN);
                    } else {
                      int selection = atoi(bbsclients[i].input);
                      Dir dir = SPIFFS.openDir(((BBSFileClient *)(bbsclients[i].data))->path);
                      if (selection > 0 && selection < 255) {
                        int readIdx = 0;
                        bool isDirectory = false;
                        bool fileFound = false;

                        pathTokCount = 0;
                        lastPathComponent[0] = 0;
                        currentPathComponent[0] = 0;

                        strcpy(tokBuffer, ((BBSFileClient *)(bbsclients[i].data))->path);
                        token = strtok(tokBuffer, "/");

                        while (token != NULL) {
                          pathTokCount++;
                          token = strtok(NULL, "/");
                        }

                        // We have to iterate over files until we reach the same index as the selection from the user
                        while (dir.next() && readIdx <= selection && !fileFound) {
                          if (strncmp(dir.fileName().c_str(), ((BBSFileClient *)(bbsclients[i].data))->path, strlen(((BBSFileClient *)(bbsclients[i].data))->path)) == 0) {
                            fileTokCount = 0;
                            strcpy(tokBuffer, (char *)dir.fileName().c_str());
                            token = strtok(tokBuffer, "/");
                            while (token != NULL) {
                              fileTokCount++;
                              token = strtok(NULL, "/");
                              if (fileTokCount == pathTokCount) {
                                strcpy(currentPathComponent, token);
                              }
                            }

                            if (fileTokCount == pathTokCount + 1) { // a file within our current path
                              readIdx++;
                              isDirectory = false;
                            } else if (fileTokCount > pathTokCount + 1) { // path to a file deeper than our current path
                              if (strcmp(lastPathComponent, currentPathComponent) != 0) {
                                strcpy(lastPathComponent, currentPathComponent);
                                readIdx++;
                                isDirectory = true;
                              }
                            }
                          }

                          if (readIdx == selection) {
                            fileFound = true;

                            if (isDirectory) {
                              sprintf(((BBSFileClient *)(bbsclients[i].data))->path, "%s/%s", ((BBSFileClient *)(bbsclients[i].data))->path, currentPathComponent);

                              action(i, BBS_FILES);
                            } else {
                              ((BBSFileClient *)(bbsclients[i].data))->f = SPIFFS.open(dir.fileName(), "r");
                              ((BBSFileClient *)(bbsclients[i].data))->bufferPosition = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->bufferUsed = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->lineCount = 0;
                              ((BBSFileClient *)(bbsclients[i].data))->nonstop = false;
                              discardInput(i);
                              pageTextFile(i);
                              action(i, BBS_FILES, BBS_FILES_READ);
                            }
                          }
                        }

                        if (!fileFound) {
                          cprintf(i, "Invalid selection.\r\n");
                          action(i, BBS_FILES);
                        }
                      } else { // selection out of bounds
                        action(i, BBS_FILES);
                      }
                    }
                  }
                } break;
              case BBS_FILES_READ: {
                  if (!((BBSFileClient *)(bbsclients[i].data))->f) {
                    action(i, BBS_FILES);
                  } else {
                    if (!bbsclients[i].inputting || ((BBSFileClient *)(bbsclients[i].data))->nonstop) {

                      if (hasInput(i)) {
                        clearEntireLine(i); // clear the prompt
                      }

                      if (bbsclients[i].input[0] == 13) { // ENTER
                        ((BBSFileClient *)(bbsclients[i].data))->nonstop = true;
                        pageTextFile(i);
                      } else if (bbsclients[i].input[0] == 27) { // ESC
                        action(i, BBS_FILES);
                      } else {
                        pageTextFile(i);
                      }

                      discardInput(i);
                    }
                  }
                } break;

            }
            break;
          case BBS_MTNC:
            switch (bbsclients[i].stage) {
              case STAGE_INIT:
              case BBS_MTNC_READ:
                if (strlen(bbsInfo.mtnc) > 0) { // If we have a message, display it, then pause before going to main menu
                  cprintf(i, "\r\n==[ Message To Next Caller ]==================================\r\n");
                  cprintf(i, bbsInfo.mtnc);

                  actionWithPause(i, BBS_MAIN);
                } else { // Otherwise, just skip to the main menu
                  action(i, BBS_MAIN);
                }
                break;
              case BBS_MTNC_SET:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    if (strcmp(bbsclients[i].input, "nothing") != 0) {
                      snprintf(bbsInfo.mtnc, MTNC_MAX_LENGTH, "%s says:\r\n%s\r\n", bbsclients[i].user.username, bbsclients[i].input);
                      cprintf(i, "I'll let them know!\r\n");
                    } else {
                      snprintf(bbsInfo.mtnc, MTNC_MAX_LENGTH, "");
                      cprintf(i, "I'll keep quiet!\r\n");
                    }
                    actionWithPause(i, BBS_MAIN);
                  } else {
                    cprintf(i, "What would you like to say? (Max. %u characters)\r\n", MTNC_MAX_LENGTH);
                    getInput(i);
                  }
                }
                break;
            }
            break;
          case BBS_NCHAT:
            switch (bbsclients[i].stage) {
              case BBS_NCHAT_SEND:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    if (strcmp(bbsclients[i].input, "exit") != 0) {
                      if (bbsclients[i].chatbuddy == 99) {
                        for (int j = 0; j < MAX_CLIENTS; j++) {
                          if (clients[j]) {
                            cprintf(j, "\r\n#%u %s says: %s\r\n", i, bbsclients[i].user.username, bbsclients[i].input);
                          }
                        }
                        discardInput(i);
                        action(i, BBS_NCHAT, BBS_NCHAT_SEND);
                      } else {
                        cprintf(bbsclients[i].chatbuddy, "\r\n#%u %s says: %s\r\n", i, bbsclients[i].user.username, bbsclients[i].input);
                        discardInput(i);
                        action(i, BBS_NCHAT, BBS_NCHAT_SEND);
                      }
                    } else {
                      action(i, BBS_MAIN);
                    }
                  } else {
                    getInput(i);
                  }
                }
                break;
              case BBS_NCHAT_SELECT:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    if (strcmp(bbsclients[i].input, "all") == 0) {
                      bbsclients[i].chatbuddy = 99;
                      cprintf(i, "What would you like to say?\r\n(Max. %u characters; type 'exit' to quit)\r\n", MTNC_MAX_LENGTH);
                      action(i, BBS_NCHAT, BBS_NCHAT_SEND);
                    } else {
                      bbsclients[i].chatbuddy = bbsclients[i].input[0] - 48;
                      if (!clients[bbsclients[i].chatbuddy]) {
                        cprintf(i, "\r\nNo caller at node %d, please select a different node!\r\n", bbsclients[i].chatbuddy);
                        discardInput(i);
                        action(i, BBS_NCHAT, BBS_NCHAT_SELECT);
                        break;
                      } else {
                        cprintf(i, "What would you like to say?\r\n(Max. %u characters; type 'exit' to quit)\r\n", MTNC_MAX_LENGTH);
                        action(i, BBS_NCHAT, BBS_NCHAT_SEND);
                      }
                    }
                  } else {
                    cprintf(i, "\r\n---- Node Chat ----\r\n\r\n");
                    cprintf(i, "Which Node would you like to chat with? (0-%d or all)\r\n", MAX_CLIENTS - 1);
                    getInput(i);
                  }
                }
                break;
            }
            break;
          case BBS_MAIN:
            switch (bbsclients[i].stage) {
              case STAGE_INIT:
                cprintf(i, "Main Menu\r\n");
                cprintf(i, "1) Log Out\r\n");
                cprintf(i, "2) File Library\r\n");
                cprintf(i, "3) Who's Online\r\n");
                cprintf(i, "4) Node Chat\r\n");
                cprintf(i, "5) Leave a Message To Next Caller\r\n");
                getInput(i);
                action(i, BBS_MAIN, BBS_MAIN_SELECT);
                break;
              case BBS_MAIN_SELECT:
                if (!bbsclients[i].inputting) {
                  if (hasInput(i)) {
                    switch (bbsclients[i].input[0]) {
                      case '1':
                        action(i, BBS_LOGOUT);
                        break;
                      case '2':
                        action(i, BBS_FILES);
                        if (bbsclients[i].data != NULL) free(bbsclients[i].data);
                        bbsclients[i].data = malloc(sizeof(struct BBSFileClient));
                        strcpy(((BBSFileClient *)(bbsclients[i].data))->path, "/files");
                        break;
                      case '3':
                        action(i, BBS_USER, BBS_USER_WHOS_ONLINE);
                        break;
                      case '4':
                        action(i, BBS_NCHAT, BBS_NCHAT_SELECT);
                        break;
                      case '5':
                        action(i, BBS_MTNC, BBS_MTNC_SET);
                        break;
                    }
                  }
                }
                break;
            }
            break;
          case BBS_LOGOUT:
            cprintf(i, "\r\nNO CARRIER\r\n\r\n");
            clients[i].stop();
            break;
          case BBS_PAUSE:
            if (!bbsclients[i].inputting) {
              if (hasInput(i)) {
                clearEntireLine(i);
                cprintf(i, "\r\n\r\n");
                action(i, bbsclients[i].nextAction, bbsclients[i].nextStage);
              } else {
                cprintf(i, "\r\n---- Press any key to continue ----");
                getInputSingle(i);
              }
            }
            break;
        }

        // See if we have any data pending
        result = clients[i].read();
        if (result != 255) {
          if (bbsclients[i].inputting) {
            if (bbsclients[i].inputSingle) {
              bbsclients[i].input[bbsclients[i].inputPos++] = result;
              bbsclients[i].inputting = false;
              bbsclients[i].input[bbsclients[i].inputPos] = 0;
              result = 0; // suppress echo
            } else if (result == 127 || result == 8) { // handle del / backspace
              if (bbsclients[i].inputPos > 0) {
                result = 8; // convert DEL (127) to backspace (8)
                bbsclients[i].input[bbsclients[i].inputPos] = 0;
                bbsclients[i].inputPos--;
                // backspace
                clients[i].write((char)result);
                // white space
                clients[i].write(' ');
                // final backspace will occur w/ normal echo code, below
              } else result = 0;
            } else if (result == 13) {
              if (bbsclients[i].inputPos > 0) {
                bbsclients[i].inputting = false;
                bbsclients[i].input[bbsclients[i].inputPos] = 0;
                clients[i].write("\r\n");
              }
              clients[i].flush();
            } else {
              if (bbsclients[i].inputPos + 1 < MAX_INPUT) {
                bbsclients[i].input[bbsclients[i].inputPos++] = result;
                if (bbsclients[i].inputEcho) result = bbsclients[i].inputEcho;
              } else result = 0;
            }

            // echo input to client
            if (result) clients[i].write((char)result);
          }
        }
      }
    }
    else {
      clients[i] = server.available();
      if (clients[i]) {
        digitalWrite(CONNECT_INDICATOR_PIN, HIGH);
        connectIndicatorMillis = millis();
        // Send telnet configuration - we'll handle echoes, and we do not want to be in linemode.
        clients[i].write(255); // IAC
        clients[i].write(251); // WILL
        clients[i].write(1);   // ECHO

        clients[i].write(255); // IAC
        clients[i].write(251); // WILL
        clients[i].write(3);   // suppress go ahead

        clients[i].write(255); // IAC
        clients[i].write(252); // WONT
        clients[i].write(34);  // LINEMODE

        // reset bbs client data
        bbsclients[i].inputting = false;
        bbsclients[i].inputPos = 0;
        bbsclients[i].inputEcho = 0;
        bbsclients[i].inputSingle = false;
        bbsclients[i].input[0] = 0;
        bbsclients[i].action = BBS_LOGIN;
        bbsclients[i].stage = STAGE_INIT;
        bbsclients[i].data = (void *)NULL;
        strcpy(bbsclients[i].user.username, "Logging in...");
        // Send the title file
        sendTextFile(clients[i], "/title.ans");

        cprintf(i, "You are caller #%u today.\r\n", bbsInfo.callersToday, bbsInfo.callersTotal);
        cprintf(i, "Log in as 'guest', or type 'new' to create an account.\r\n");
        clients[i].flush(); // discard initial telnet data from client

        // update and save stats
        bbsInfo.callersTotal++;
        bbsInfo.callersToday++;
        persistBBSInfo();
      }
    }
  }

  // Catch extra callers
  WiFiClient client = server.available();
  if (client) {
    sprintf(buf, "All nodes are currently in use.\r\n");
    client.write((uint8_t *)buf, strlen(buf));
    client.stop();
  }

  // reset connect indicator pin
  if (connectIndicatorMillis + CONNECT_INDICATOR_DURATION <= millis()) {
    digitalWrite(CONNECT_INDICATOR_PIN, LOW);
  }

  // Let ESP8266 do some stuff
  yield();
}

