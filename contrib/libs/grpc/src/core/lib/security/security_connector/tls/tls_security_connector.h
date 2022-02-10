/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_TLS_TLS_SECURITY_CONNECTOR_H
#define GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_TLS_TLS_SECURITY_CONNECTOR_H

#include <grpc/support/port_platform.h>

#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/security/context/security_context.h"
#include "src/core/lib/security/credentials/tls/grpc_tls_credentials_options.h"

#define GRPC_TLS_TRANSPORT_SECURITY_TYPE "tls"

namespace grpc_core {

// TLS channel security connector.
class TlsChannelSecurityConnector final
    : public grpc_channel_security_connector {
 public:
  // static factory method to create a TLS channel security connector.
  static grpc_core::RefCountedPtr<grpc_channel_security_connector>
  CreateTlsChannelSecurityConnector(
      grpc_core::RefCountedPtr<grpc_channel_credentials> channel_creds,
      grpc_core::RefCountedPtr<grpc_call_credentials> request_metadata_creds,
      const char* target_name, const char* overridden_target_name,
      tsi_ssl_session_cache* ssl_session_cache);

  TlsChannelSecurityConnector(
      grpc_core::RefCountedPtr<grpc_channel_credentials> channel_creds,
      grpc_core::RefCountedPtr<grpc_call_credentials> request_metadata_creds,
      const char* target_name, const char* overridden_target_name);
  ~TlsChannelSecurityConnector() override;

  void add_handshakers(const grpc_channel_args* args,
                       grpc_pollset_set* interested_parties,
                       grpc_core::HandshakeManager* handshake_mgr) override;

  void check_peer(tsi_peer peer, grpc_endpoint* ep,
                  grpc_core::RefCountedPtr<grpc_auth_context>* auth_context,
                  grpc_closure* on_peer_checked) override;

  int cmp(const grpc_security_connector* other_sc) const override;

  bool check_call_host(y_absl::string_view host, grpc_auth_context* auth_context,
                       grpc_closure* on_call_host_checked,
                       grpc_error** error) override;

  void cancel_check_call_host(grpc_closure* on_call_host_checked,
                              grpc_error* error) override;

 private:
  // Initialize SSL TSI client handshaker factory.
  grpc_security_status InitializeHandshakerFactory(
      tsi_ssl_session_cache* ssl_session_cache);

  // A util function to create a new client handshaker factory to replace
  // the existing one if exists.
  grpc_security_status ReplaceHandshakerFactory(
      tsi_ssl_session_cache* ssl_session_cache);

  // gRPC-provided callback executed by application, which servers to bring the
  // control back to gRPC core.
  static void ServerAuthorizationCheckDone(
      grpc_tls_server_authorization_check_arg* arg);

  // A util function to process server authorization check result.
  static grpc_error* ProcessServerAuthorizationCheckResult(
      grpc_tls_server_authorization_check_arg* arg);

  // A util function to create a server authorization check arg instance.
  static grpc_tls_server_authorization_check_arg*
  ServerAuthorizationCheckArgCreate(void* user_data);

  // A util function to destroy a server authorization check arg instance.
  static void ServerAuthorizationCheckArgDestroy(
      grpc_tls_server_authorization_check_arg* arg);

  // A util function to refresh SSL TSI client handshaker factory with a valid
  // credential.
  grpc_security_status RefreshHandshakerFactory();

  grpc_core::Mutex mu_;
  grpc_closure* on_peer_checked_;
  TString target_name_;
  TString overridden_target_name_;
  tsi_ssl_client_handshaker_factory* client_handshaker_factory_ = nullptr;
  grpc_tls_server_authorization_check_arg* check_arg_;
  grpc_core::RefCountedPtr<grpc_tls_key_materials_config> key_materials_config_;
};

// TLS server security connector.
class TlsServerSecurityConnector final : public grpc_server_security_connector {
 public:
  // static factory method to create a TLS server security connector.
  static grpc_core::RefCountedPtr<grpc_server_security_connector>
  CreateTlsServerSecurityConnector(
      grpc_core::RefCountedPtr<grpc_server_credentials> server_creds);

  explicit TlsServerSecurityConnector(
      grpc_core::RefCountedPtr<grpc_server_credentials> server_creds);
  ~TlsServerSecurityConnector() override;

  void add_handshakers(const grpc_channel_args* args,
                       grpc_pollset_set* interested_parties,
                       grpc_core::HandshakeManager* handshake_mgr) override;

  void check_peer(tsi_peer peer, grpc_endpoint* ep,
                  grpc_core::RefCountedPtr<grpc_auth_context>* auth_context,
                  grpc_closure* on_peer_checked) override;

  int cmp(const grpc_security_connector* other) const override;

 private:
  // Initialize SSL TSI server handshaker factory.
  grpc_security_status InitializeHandshakerFactory();

  // A util function to create a new server handshaker factory to replace the
  // existing once if exists.
  grpc_security_status ReplaceHandshakerFactory();

  // A util function to refresh SSL TSI server handshaker factory with a valid
  // credential.
  grpc_security_status RefreshHandshakerFactory();

  grpc_core::Mutex mu_;
  tsi_ssl_server_handshaker_factory* server_handshaker_factory_ = nullptr;
  grpc_core::RefCountedPtr<grpc_tls_key_materials_config> key_materials_config_;
};

// ---- Functions below are exposed for testing only -----------------------

/** The |TlsFetchKeyMaterials| API ensures that |key_materials_config| has a
 *  non-empty pem-key-cert pair list. This is done as follows:
 *  - if |options| is equipped with a credential reload config, then this
 *    methods uses credential reloading to populate |key_materials_config|, and
 *    afterwards it populates |reload_status| with the status of this operation.
 *    In particular, any data stored in |key_materials_config| is overwritten.
 *  - if |options| has no credential reload config, then:
 *    - if |key_materials_config| already has a non-empty pem-key-cert pair
 *      list or is called by a client, then the method returns |GRPC_STATUS_OK|.
 *    - if |key_materials_config| has an empty pem-key-cert pair list and is
 *      called by a server, then the method return an error code.
 *
 *  The arguments are detailed below:
 *  - key_materials_config: a key materials config that will be populated by the
 *    method on success; the caller should not pass in nullptr. Any data held by
 *    the config will be overwritten.
 *  - options: the TLS credentials options whose credential reloading config
 *    will be used to populate |key_materials_config|.
 *  - is_server: true denotes that this method is called by a server, and
 *    false denotes that this method is called by a client.
 *  - status: the status of the credential reloading after the method
 *    returns; the caller should not pass in nullptr. **/
grpc_status_code TlsFetchKeyMaterials(
    const grpc_core::RefCountedPtr<grpc_tls_key_materials_config>&
        key_materials_config,
    const grpc_tls_credentials_options& options, bool is_server,
    grpc_ssl_certificate_config_reload_status* status);

// TlsCheckHostName checks if |peer_name| matches the identity information
// contained in |peer|. This is AKA hostname check.
grpc_error* TlsCheckHostName(const char* peer_name, const tsi_peer* peer);

}  // namespace grpc_core

#endif /* GRPC_CORE_LIB_SECURITY_SECURITY_CONNECTOR_TLS_TLS_SECURITY_CONNECTOR_H \
        */
