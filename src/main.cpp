#include <Arduino.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh_esp32.h>
#include "credentials.hpp"

#define BUF_SIZE 2048

static const uint8_t SSHPORT = 22;
static bool authenticated = false;

void setup() {
  delay(1 * 1000);
  Serial.begin(115200);
  Serial2.begin(115200);
  Serial2.setTimeout(0);
  esp_netif_init();
  boolean fsGood = SPIFFS.begin();
  if (!fsGood) {
    printf("%% No formatted SPIFFS filesystem found to mount.\n");
    printf("%% Formatting...\n");
    fsGood = SPIFFS.format();
    if (fsGood)
      SPIFFS.begin();
  }
  if (!fsGood) {
    printf("%% Aborting now.\n");
    ESP.restart();
  }
  printf("%% Mounted SPIFFS used=%d total=%d\r\n", SPIFFS.usedBytes(),
         SPIFFS.totalBytes());
  WiFi.mode(WIFI_MODE_STA);
  WiFi.setHostname("SerialSSHProxy");
  WiFi.begin(WIFISSID, WIFIPASS);
  WiFi.waitForConnectResult();
  if (WiFi.status() != WL_CONNECTED)
    ESP.restart();
  printf("WiFi IP-Address is %s\n", WiFi.localIP().toString().c_str());
  libssh_begin();
}

int ex_main();

void loop() {
  // put your main code here, to run repeatedly:
  int res = ex_main();
  printf("execution result: %d\n", res);
  if (res > 0) {
    printf("Rebooting due to execution error");
    ESP.restart();
  }
}

static int auth_password(const char* user, const char* password) {
  int cmp;

  cmp = strcmp(user, SSHUSER);
  if (cmp != 0) {
    return 0;
  }
  cmp = strcmp(password, SSHPASS);
  if (cmp != 0) {
    return 0;
  }

  authenticated = true;
  return 1;  // authenticated
}

static int authenticate(ssh_session session) {
  ssh_message message;

  do {
    message = ssh_message_get(session);
    if (!message)
      break;
    switch (ssh_message_type(message)) {
      case SSH_REQUEST_AUTH:
        switch (ssh_message_subtype(message)) {
          case SSH_AUTH_METHOD_PASSWORD:
            printf("User %s wants to auth with pass %s\n",
                   ssh_message_auth_user(message),
                   ssh_message_auth_password(message));
            if (auth_password(ssh_message_auth_user(message),
                              ssh_message_auth_password(message))) {
              ssh_message_auth_reply_success(message, 0);
              ssh_message_free(message);
              return 1;
            }
            ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
            // not authenticated, send default message
            ssh_message_reply_default(message);
            break;
          case SSH_AUTH_METHOD_NONE:
          default:
            printf("User %s wants to auth with unknown auth %d\n",
                   ssh_message_auth_user(message),
                   ssh_message_subtype(message));
            ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
            ssh_message_reply_default(message);
            break;
        }
        break;
      default:
        ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
        ssh_message_reply_default(message);
    }
    ssh_message_free(message);
  } while (1);
  return 0;
}

int ex_main() {
  ssh_session session;
  ssh_bind sshbind;
  ssh_message message;
  ssh_channel chan = 0;
  int auth = 0;
  int shell = 0;
  int i;
  int r;

  sshbind = ssh_bind_new();
  session = ssh_new();

  ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT_STR,
                       String(SSHPORT).c_str());

  ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_RSAKEY,
                       "/spiffs/"
                       "hostkey_rsa");

  if (ssh_bind_listen(sshbind) < 0) {
    printf("Error listening to socket: %s\n", ssh_get_error(sshbind));
    return 1;
  }
  printf("Started sample libssh sshd on port %d\n", SSHPORT);
  printf("You can login as the user %s with the password %s\n", SSHUSER,
         SSHPASS);
  r = ssh_bind_accept(sshbind, session);
  if (r == SSH_ERROR) {
    printf("Error accepting a connection: %s\n", ssh_get_error(sshbind));
    return 1;
  }
  if (ssh_handle_key_exchange(session)) {
    printf("ssh_handle_key_exchange: %s\n", ssh_get_error(session));
    return 1;
  }

  /* proceed to authentication */
  auth = authenticate(session);
  if (!auth || !authenticated) {
    printf("Authentication error: %s\n", ssh_get_error(session));
    ssh_disconnect(session);
    return 1;
  }

  /* wait for a channel session */
  do {
    message = ssh_message_get(session);
    if (message) {
      if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN &&
          ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
        chan = ssh_message_channel_request_open_reply_accept(message);
        ssh_message_free(message);
        break;
      } else {
        ssh_message_reply_default(message);
        ssh_message_free(message);
      }
    } else {
      break;
    }
  } while (!chan);

  if (!chan) {
    printf("Error: cleint did not ask for a channel session (%s)\n",
           ssh_get_error(session));
    ssh_finalize();
    return 1;
  }

  /* wait for a shell */
  do {
    message = ssh_message_get(session);
    if (message != NULL) {
      if (ssh_message_type(message) == SSH_REQUEST_CHANNEL &&
          (ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL ||
           ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_PTY)) {
        shell = 1;
        ssh_message_channel_request_reply_success(message);
        ssh_message_free(message);
        break;
      }
      ssh_message_reply_default(message);
      ssh_message_free(message);
    } else {
      break;
    }
  } while (!shell);

  if (!shell) {
    printf("Error: No shell requested (%s)\n", ssh_get_error(session));
    return 1;
  }

  printf("Client connected!\n");
  ssh_channel_write(chan, "Connected!\r\n", 13);
  char sshbuf[BUF_SIZE];
  char serialbuf[BUF_SIZE];
  do {
    if (ssh_channel_poll(chan, 0) > 0) {
      i = ssh_channel_read_nonblocking(chan, sshbuf, sizeof(sshbuf), 0);
      if (i > 0) {
        // disconnect ssh connection when pressing CTRL-G
        if (i == 1 && *sshbuf == '\7')
          break;
        // if (*sshbuf == '\3' || *sshbuf == '\4') break;
        Serial2.write(sshbuf, i);
      }
    }
    if (Serial2.available()) {
      i = Serial2.readBytes(serialbuf, sizeof(serialbuf));
      if (i > 0) {
        ssh_channel_write(chan, serialbuf, i);
      }
    }
  } while (ssh_channel_is_open(chan));
  ssh_channel_close(chan);
  ssh_disconnect(session);
  ssh_bind_free(sshbind);
  ssh_finalize();
  return 0;
}
