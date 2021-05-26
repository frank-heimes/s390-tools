/*
 * zkey-kmip - KMIP zkey KMS plugin
 *
 * Copyright IBM Corp. 2021
 *
 * s390-tools is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/utsname.h>

#include <openssl/objects.h>

#include "lib/zt_common.h"
#include "lib/util_libc.h"
#include "lib/util_panic.h"
#include "lib/util_path.h"
#include "lib/util_base.h"

#include "zkey-kmip.h"
#include "../kms-plugin.h"
#include "../cca.h"
#include "../utils.h"
#include "../pkey.h"
#include "../properties.h"

#include "libseckey/sk_utilities.h"

#define _set_error(ph, fmt...)	plugin_set_error(&(ph)->pd, fmt)

/**
 * Informs a KMS plugin that it is bound to a zkey repository.
 *
 * Note: This function is called before kms_initialize()!
 *
 * @param config_path       name of a directory where the KMS plugin can store
 *                          its configuration and other files it needs to store
 *
 * @returns 0 on success, or a negative errno in case of an error.
 */
int kms_bind(const char *UNUSED(config_path))
{
	return 0;
}

/**
 * Checks if the plugin configuration is complete. Sets the appropriate flags
 * in the plugin handle
 *
 * @param ph                the plugin handle
 */
static void _check_config_complete(struct plugin_handle *ph)
{
	ph->apqns_configured =
		plugin_check_property(&ph->pd, KMIP_CONFIG_APQNS) &&
		plugin_check_property(&ph->pd, KMIP_CONFIG_APQN_TYPE) &&
		ph->card_type != CARD_TYPE_ANY;

	ph->identity_key_generated =
		plugin_check_property(&ph->pd, KMIP_CONFIG_IDENTITY_KEY) &&
		plugin_check_property(&ph->pd,
				      KMIP_CONFIG_IDENTITY_KEY_ALGORITHM) &&
		plugin_check_property(&ph->pd, KMIP_CONFIG_IDENTITY_KEY_PARAMS);

	ph->client_cert_avail =
		plugin_check_property(&ph->pd,
				      KMIP_CONFIG_CLIENT_CERTIFICATE) &&
		plugin_check_property(&ph->pd,
				      KMIP_CONFIG_CLIENT_CERT_ALGORITHM);

	ph->config_complete = ph->apqns_configured &&
			      ph->identity_key_generated &&
			      ph->client_cert_avail;
}

/**
 * Returns a textual name of the specified card type.
 *
 * @param card_type          the card type
 *
 * @returns a constant string, or NULL if an invalid card type is specified
 */
static const char *_card_type_to_str(enum card_type card_type)
{
	switch (card_type) {
	case CARD_TYPE_CCA:
		return KMIP_APQN_TYPE_CCA;
	case CARD_TYPE_EP11:
		return KMIP_APQN_TYPE_EP11;
	default:
		return NULL;
	}
}

/**
 * Returns the card type for the textual name of the card type.
 *
 * @param card_type          the card type as string
 *
 * @returns the card type value, or CARD_TYPE_ANY if unknown
 */
static enum card_type _card_type_from_str(const char *card_type)
{
	if (strcmp(card_type, KMIP_APQN_TYPE_CCA) == 0)
		return CARD_TYPE_CCA;
	if (strcmp(card_type, KMIP_APQN_TYPE_EP11) == 0)
		return CARD_TYPE_EP11;

	return CARD_TYPE_ANY;
}

/**
 * Unloads the CCA library
 *
 * @param ph                the plugin handle
 */
static void _terminate_cca_library(struct plugin_handle *ph)
{
	if (ph->cca_lib.cca_lib != NULL)
		dlclose(ph->cca_lib.cca_lib);
	ph->cca_lib.cca_lib = NULL;
}

/*
 * Sets up the CCA library structure in the handle. Load the CCA library
 * and selects one of the associated APQNs.
 *
 * @param ph                the plugin handle
 * @param apqns             the associated APQNs
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _setup_cca_library(struct plugin_handle *ph, const char *apqns)
{
	int rc;

	_terminate_cca_library(ph);

	rc = select_cca_adapter_by_apqns(&ph->pd, apqns, &ph->cca);
	if (rc != 0) {
		_set_error(ph, "Failed to select one of the associated APQNs: "
			   "%s", apqns);
		_terminate_cca_library(ph);
	}

	ph->cca_lib.cca_lib = ph->cca.lib_csulcca;

	return rc;
}

/**
 * Unloads the Ep11 library
 *
 * @param ph                the plugin handle
 */
static void _terminate_ep11_library(struct plugin_handle *ph)
{
	if (ph->ep11.lib_ep11 == NULL)
		return;

	if (ph->ep11_lib.target != 0) {
		free_ep11_target_for_apqn(&ph->ep11, ph->ep11_lib.target);
		ph->ep11_lib.target = 0;
	}
	ph->ep11_lib.ep11_lib = NULL;

	if (ph->ep11.lib_ep11 != NULL)
		dlclose(ph->ep11.lib_ep11);
	memset(&ph->ep11, 0, sizeof(ph->ep11));
}

/*
 * Sets up the EP11 library structure in the handle. Load the EP11 library
 * and sets up the EP11 target with the APQNs specified
 *
 * @param ph                the plugin handle
 * @param apqns             the associated APQNs
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _setup_ep11_library(struct plugin_handle *ph, const char *apqns)
{
	unsigned int card, domain;
	bool selected = false;
	char **apqn_list;
	int rc, i;

	rc = load_ep11_library(&ph->ep11, ph->pd.verbose);
	if (rc != 0) {
		_set_error(ph, "Failed load the EP11 host library");
		return rc;
	}

	apqn_list = str_list_split(apqns);
	for (i = 0; apqn_list[i] != NULL; i++) {
		if (sscanf(apqn_list[i], "%x.%x", &card, &domain) != 2)
			continue;

		if (sysfs_is_apqn_online(card, domain, CARD_TYPE_EP11) != 1)
			continue;

		rc = get_ep11_target_for_apqn(&ph->ep11, card, domain,
					      &ph->ep11_lib.target,
					      ph->pd.verbose);
		if (rc != 0) {
			_set_error(ph, "Failed to get EP11 target for "
				   "APQN %02x.%04x: %s", card, domain,
				   strerror(-rc));
			goto out;
		}

		selected = true;
		break;
	}

	if (!selected) {
		_set_error(ph, "None of the associated APQNs is "
			   "available: %s", apqns);
		rc = -ENODEV;
		goto out;
	}

	pr_verbose(&ph->pd, "Selected APQN %02x.%04x", card, domain);

	ph->ep11_lib.ep11_lib = ph->ep11.lib_ep11;

out:
	if (apqn_list != NULL)
		str_list_free_string_array(apqn_list);

	if (rc != 0)
		_terminate_ep11_library(ph);

	return rc;
}

/**
 * Terminates the external secure key library structure in the handle
 * and the OpenSSL secure key interface.
 *
 * @param ph                the plugin handle
 */
static void _terminate_ext_lib(struct plugin_handle *ph)
{
	if (ph->ext_lib.type != 0)
		SK_OPENSSL_term();

	switch (ph->ext_lib.type) {
	case SK_EXT_LIB_CCA:
		_terminate_cca_library(ph);
		ph->ext_lib.cca = NULL;
		break;

	case SK_EXT_LIB_EP11:
		_terminate_ep11_library(ph);
		ph->ext_lib.ep11 = NULL;
		break;

	default:
		break;
	}

	ph->ext_lib.type = 0;
}

/**
 * Initializes the external secure key library structure in the handle
 * with the information from the associated APQNs. Also initializes the
 * OpenSSL secure key interface.
 *
 * @param ph                the plugin handle
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _setup_ext_lib(struct plugin_handle *ph)
{
	char *apqns;
	int rc = 0;

	if (ph->ext_lib.type != 0)
		return 0;

	if (!ph->apqns_configured) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first configure the APQNs used with this plugin.");
		return -EINVAL;
	}

	apqns = properties_get(ph->pd.properties, KMIP_CONFIG_APQNS);
	if (apqns == NULL) {
		_set_error(ph, "No APQN are associated with the plugin.");
		return -ENODEV;
	}

	pr_verbose(&ph->pd, "Associated APQNs: %s", apqns);

	switch (ph->card_type) {
	case CARD_TYPE_CCA:
		rc = _setup_cca_library(ph, apqns);
		if (rc != 0)
			goto out;

		ph->ext_lib.type = SK_EXT_LIB_CCA;
		ph->ext_lib.cca = &ph->cca_lib;
		break;

	case CARD_TYPE_EP11:
		rc = _setup_ep11_library(ph, apqns);
		if (rc != 0)
			goto out;

		ph->ext_lib.type = SK_EXT_LIB_EP11;
		ph->ext_lib.ep11 = &ph->ep11_lib;
		break;

	default:
		_set_error(ph, "The configuration is incomplete, you must "
			   "first configure the APQNs used with this plugin.");
		return -EINVAL;
	}

	rc = SK_OPENSSL_init(ph->pd.verbose);
	if (rc != 0)
		_terminate_ext_lib(ph);

out:
	free(apqns);

	return rc;
}

/**
 * Initializes a KMS plugin for usage by zkey. When a repository is bound to a
 * KMS plugin, zkey calls this function when opening the repository.
 *
 * @param config_path       name of a directory where the KMS plugin can store
 *                          its configuration and other files it needs to store
 * @param verbose           if true, the plugin should write verbose or debug
 *                          messages to stderr during further processing.
 *
 * @returns a KMS plugin handle, or NULL in case of an error.
 */
kms_handle_t kms_initialize(const char *config_path, bool verbose)
{
	struct plugin_handle *ph;
	char *apqn_type = NULL;
	int rc;

	util_assert(config_path != NULL, "Internal error: config_path is NULL");

	ph = util_malloc(sizeof(struct plugin_handle));
	memset(ph, 0, sizeof(struct plugin_handle));

	rc = plugin_init(&ph->pd, "zkey-kmip", config_path,
			 KMIP_CONFIG_FILE, verbose);
	if (rc != 0)
		goto error;

	_check_config_complete(ph);
	pr_verbose(&ph->pd, "Plugin configuration is %scomplete",
		   ph->config_complete ? "" : "in");

	ph->card_type = CARD_TYPE_ANY;
	apqn_type = properties_get(ph->pd.properties, KMIP_CONFIG_APQN_TYPE);
	if (apqn_type != NULL) {
		ph->card_type = _card_type_from_str(apqn_type);
		free(apqn_type);
		if (ph->card_type == CARD_TYPE_ANY) {
			pr_verbose(&ph->pd, "APQN type invalid: %s", apqn_type);
			goto error;
		}
	}

	ph->identity_secure_key = properties_get(ph->pd.properties,
						 KMIP_CONFIG_IDENTITY_KEY);

	return (kms_handle_t)ph;

error:
	if (strlen(ph->pd.error_msg) > 0)
		warnx("%s", ph->pd.error_msg);

	kms_terminate(ph);
	return NULL;
}

/**
 * Terminates the use of a KMS plugin. When a repository is bound to a KMS
 * plugin, zkey calls this function when closing the repository.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_terminate(const kms_handle_t handle)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");

	pr_verbose(&ph->pd, "Plugin terminating");

	if (ph->identity_secure_key != NULL)
		free((void *)ph->identity_secure_key);

	_terminate_ext_lib(ph);
	plugin_term(&ph->pd);
	free(ph);

	return 0;
}

/**
 * Returns a textual message about the last occurred error that occurred in the
 * last called KMS plugin function. If no error occurred (i.e. the last plugin
 * function returned rc = 0), then NULL is returned.
 * The returned string is static or contained within the handle. It is valid
 * only until the next KMS plugin function is called.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 *
 * @returns an error message of NULL
 */
const char *kms_get_last_error(const kms_handle_t handle)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");

	pr_verbose(&ph->pd, "Last error: '%s'", ph->pd.error_msg);

	if (strlen(ph->pd.error_msg) == 0)
		return NULL;

	return ph->pd.error_msg;
}

/**
 * Returns true if the KMS plugin supports the specified key type.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_type          the zkey key type, euch as 'CCA-AESDATA',
 *                          'CCA-AESCIPHER', 'EP11-AES'.
 *
 * @returns true if the KMS plugin supports the key type, false otherwise.
 */
bool kms_supports_key_type(const kms_handle_t handle,
			   const char *key_type)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(key_type != NULL, "Internal error: key_type is NULL");

	plugin_clear_error(&ph->pd);

	switch (ph->card_type) {
	case CARD_TYPE_CCA:
		if (strcasecmp(key_type, KEY_TYPE_CCA_AESDATA) == 0)
			return true;
		if (strcasecmp(key_type, KEY_TYPE_CCA_AESCIPHER) == 0)
			return true;
		break;
	case CARD_TYPE_EP11:
		if (strcasecmp(key_type, KEY_TYPE_EP11_AES) == 0)
			return true;
		break;
	default:
		if (strcasecmp(key_type, KEY_TYPE_CCA_AESDATA) == 0)
			return true;
		if (strcasecmp(key_type, KEY_TYPE_CCA_AESCIPHER) == 0)
			return true;
		if (strcasecmp(key_type, KEY_TYPE_EP11_AES) == 0)
			return true;
		break;
	}

	return false;
}

/**
 * Displays information about the KMS Plugin and its current configuration on
 * stdout.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_display_info(const kms_handle_t handle)
{
	struct plugin_handle *ph = handle;
	X509 *cert = NULL;
	char *tmp = NULL;
	bool rsa;
	BIO *b;

	util_assert(handle != NULL, "Internal error: handle is NULL");

	pr_verbose(&ph->pd, "Display Info");

	plugin_clear_error(&ph->pd);

	tmp = properties_get(ph->pd.properties,
			     KMIP_CONFIG_IDENTITY_KEY_ALGORITHM);
	if (tmp != NULL) {
		printf("  Identity key:         %s", tmp);
		rsa = strcmp(tmp, KMIP_KEY_ALGORITHM_RSA) == 0;
		free(tmp);
		tmp = properties_get(ph->pd.properties,
				     KMIP_CONFIG_IDENTITY_KEY_PARAMS);
		if (tmp != NULL) {
			printf(" (%s%s)", tmp, rsa ? " bits" : "");
			free(tmp);
		}
		printf("\n");
	} else {
		printf("  Identity key:         (configuration required)\n");
	}

	tmp = properties_get(ph->pd.properties,
			     KMIP_CONFIG_CLIENT_CERTIFICATE);
	if (tmp != NULL) {
		SK_UTIL_read_x509_certificate(tmp, &cert);
		free(tmp);
		if (cert != NULL) {
			b = BIO_new_fp(stdout, BIO_NOCLOSE);
			BIO_printf(b,
				   "  Client certificate:   Subject:\n");
			X509_NAME_print_ex(b, X509_get_subject_name(cert), 26,
					   XN_FLAG_SEP_MULTILINE);
			BIO_printf(b,
				   "\n                        Issuer:\n");
			X509_NAME_print_ex(b, X509_get_issuer_name(cert), 26,
					   XN_FLAG_SEP_MULTILINE);
			BIO_printf(b, "\n                        Validity:\n");
			BIO_printf(b,
				   "                          Not before:  ");
			ASN1_TIME_print(b, X509_get0_notBefore(cert));
			BIO_printf(b,
				   "\n                          Not after:   ");
			ASN1_TIME_print(b, X509_get0_notAfter(cert));
			BIO_printf(b,
				   "\n                        Serial Number: ");
			i2a_ASN1_INTEGER(b, X509_get0_serialNumber(cert));
			BIO_printf(b, "\n");
			BIO_free(b);
			X509_free(cert);
		} else {
			printf("  Client certificate:   (error)\n");
		}
	} else {
		printf("  Client certificate:   (configuration required)\n");
		return 0;
	}

	return 0;
}

static const struct util_opt configure_options[] = {
	{
		.flags = UTIL_OPT_FLAG_SECTION,
		.desc = "KMIP SPECIFIC OPTIONS FOR IDENTITY KEY GENERATION",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "gen-identity-key", required_argument, NULL, 'i'},
		.argument = "KEY-SPEC",
		.desc = "Generates an identity key for the KMIP plugin. The "
			"identity key is a secure ECC or RSA key. The identity "
			"key is automatically generated with the default "
			"values ECC with curve secp521r1 when a certificate "
			"signing request (CSR) or self-signed certificate is "
			"to be generated and no identity key is available. Use "
			"this option to generate or regenerate a new identity "
			"key with with specific parameters. You need to "
			"regenerate a certificate with the newly generated "
			"identity key and reregister this certificate with the "
			"KMIP server.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.flags = UTIL_OPT_FLAG_SECTION,
		.desc = "KMIP SPECIFIC OPTIONS FOR CERTIFICATE GENERATION",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "gen-csr", required_argument, NULL, 'c'},
		.argument = "CSR-PEM-FILE",
		.desc = "Generates a certificate signing request (CSR) with "
			"the identity key and stores it in the specified PEM "
			"file. Pass this CSR to a certificate authority (CA) "
			"to request a CA-signed certificate for the KMIP "
			"plugin. You need to register the certificate with the "
			"KMIP server. Registering a client certificate with "
			"the KMIP server is a manual procedure, and is "
			"specific to the KMIP server used. The KMIP server "
			"accepts communication with the KMIP plugin only after "
			"the certificate was registered. You must also specify "
			"the CA-signed certificate with the 'zkey kms "
			"configure --client-cert' option so that the KMIP "
			"plugin uses it for communicating with the KMIP."
			"server.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "gen-self-signed-cert", required_argument, NULL,
			    'C'},
		.argument = "CERT-PEM-FILE",
		.desc = "Generates a self-signed certificate with the "
			"identity key and stores it in the specified PEM "
			"file. You need to register the certificate with the "
			"KMIP server. Registering a client certificate with "
			"the KMIP server is a manual procedure, and is "
			"specific to the KMIP server used. The KMIP server "
			"accepts communication with the KMIP plugin only after "
			"the certificate was registered.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "cert-subject", required_argument, NULL, 's'},
		.argument = "SUBJECT-RDNS",
		.desc = "Specifies the subject name for generating a "
			"certificate signing request (CSR) or self-signed "
			"certificate, in the form '<type>=<value>(;<type>="
			"<value>)*[;]' with types recognized by OpenSSL.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "cert-extensions", required_argument, NULL, 'e'},
		.argument = "EXTENSIONS",
		.desc = "Specifies the certificate extensions for generating a "
			"certificate signing request (CSR) or self-signed "
			"certificate, in the form '<name>=[critical,]<value(s)>"
			" (;<name>=[critical,]<value(s)>)*[;]' with extension "
			"names and values recognized by OpenSSL. A certificate "
			"used to authenticate at a KMIP server usually needs "
			"the 'TLS Web client authentication' extended-key-"
			"usage certificate extension. Additionally, the "
			"'Common Name' field or the 'Subject Alternate Name' "
			"extension must match the host name (or IP address) of "
			"the client system. If no extended-key-usage extension "
			"is specified, then a 'TLS Web client authentication' "
			"extension ('extendedKeyUsage = clientAuth') is "
			"automatically added. If no 'Subject Alternate Name' "
			"extension is specified, then an 'Subject Alternate "
			"Name' extension with the system's host name "
			"(subjectAltName = DNS:hostname) is automatically "
			"added.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "renew-cert", required_argument, NULL, 'N'},
		.argument = "CERT-PEM-FILE",
		.desc = "Specifies an existing PEM file that contains the "
			"certificate to be renewed. The subject name and "
			"extensions of the certificate are used to generate "
			"the certificate signing request (CSR) or renewed "
			"self-signed certificate.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "csr-new-header", 0, NULL, 'n'},
		.desc = "Adds the word 'NEW' to the PEM file header and footer "
			"lines on the certificate signing request. Some "
			"software and some CAs require this marking.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "cert-validity-days", required_argument, NULL, 'd'},
		.argument = "DAYS",
		.desc = "Specifies the number of days the self-signed "
			"certificate is valid. The default is 30 days.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "cert-digest", required_argument, NULL, 'D'},
		.argument = "DIGEST",
		.desc = "Specifies the digest algorithm to use when generating "
			"a certificate signing request or self-signed "
			"certificate. The default is determined by OpenSSL.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "cert-rsa-pss", 0, NULL, 'P'},
		.desc = "Uses the RSA-PSS algorithm to sign the certificate "
			"signing request or the self-signed certificate. This "
			"option is accepted only when the identity key type is "
			"RSA, it is ignored otherwise.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.flags = UTIL_OPT_FLAG_SECTION,
		.desc = "KMIP SPECIFIC OPTIONS FOR CERTIFICATE REGISTRATION",
		.command = KMS_COMMAND_CONFIGURE,
	},
	{
		.option = { "client-cert", required_argument, NULL,
			    'r'},
		.argument = "CERT-PEM-FILE",
		.desc = "Uses a CA-signed certificate for authenticating the "
			"KMIP plugin at the KMIP server. The certificate must "
			"be registered with the KMIP server. Registering a "
			"client certificate with the KMIP server is a manual "
			"procedure, and is specific to the KMIP server used. "
			"The KMIP server accepts communication with the KMIP "
			"plugin only after the certificate has been "
			"registered.",
		.command = KMS_COMMAND_CONFIGURE,
	},
	UTIL_OPT_END,
};


/**
 * Returns a list of KMS specific command line options that zkey should accept
 * and pass to the appropriate KMS plugin function. The option list must be
 * terminated by an UTIL_OPT_END entry (see util_opt.h). The options returned
 * must not interfere with the already defined options of the zkey command.
 * Field 'command' of the returned options should either be NULL or specify
 * the command that it is for.
 *
 * If max_opts is not -1, then only up to max_opts options are allowed. If more
 * options are returned, only up to max_opts options are used by zkey.
 *
 * @param command           the command for which the KMS-specific options are
 *                          to be returned, see KMS_COMMAND_xxx defines
 * @param max_opts          maximum number of options allowed. If -1 then there
 *                          is no limit.
 *
 * @returns a list of options terminated by an UTIL_OPT_END entry, or NULL in
 * case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
const struct util_opt *kms_get_command_options(const char *command,
					       int UNUSED(max_opts))
{
	util_assert(command != NULL, "Internal error: command is NULL");

	if (strcasecmp(command, KMS_COMMAND_CONFIGURE) == 0)
		return configure_options;

	return NULL;
}

struct config_options {
	const char *generate_identity_key;
	const char *sscert_pem_file;
	const char *csr_pem_file;
	const char *cert_subject;
	const char *cert_extensions;
	const char *renew_cert_pem_file;
	bool csr_new_header;
	const char *cert_validity_days;
	const char *cert_digest;
	bool cert_rsa_pss;
	const char *client_cert;
};

/**
 * Check the specified APQns and assure that they are all of the right type.
 *
 * @param ph                the plugin handle
 * @param apqns             a list of APQNs to associate with the KMS plugin, or
 *                          NULL if no APQNs are specified.
 * @param num_apqns         number of APQNs in above array. 0 if no APQNs are
 *                          specified.
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _check_apqns(struct plugin_handle *ph, const struct kms_apqn *apqns,
			size_t num_apqns)
{
	size_t i;
	int rc;

	if (num_apqns == 0)
		return 0;

	if (ph->card_type == CARD_TYPE_ANY) {
		/*
		 * No APQNs configured yet, accept any APQN type, but all must
		 * be of the same type.
		 */
		ph->card_type = sysfs_get_card_type(apqns[0].card);
		if (ph->card_type == CARD_TYPE_ANY) {
			_set_error(ph, "The APQN %02x.%04x is not available or "
				   "has an unsupported type", apqns[0].card,
				   apqns[0].domain);
			return -EINVAL;
		}
	}

	pr_verbose(&ph->pd, "Check APQNs for card type %s",
		   _card_type_to_str(ph->card_type));

	for (i = 0; i < num_apqns; i++) {
		rc = sysfs_is_apqn_online(apqns[i].card, apqns[i].domain,
					  ph->card_type);
		if (rc != 1) {
			_set_error(ph, "APQN %02x.%04x is not of the right "
				   "type. The plugin is configured to use "
				   "APQNs of type %s", apqns[i].card,
				   apqns[i].domain,
				   _card_type_to_str(ph->card_type));

			return -EINVAL;
		}
	}

	return 0;
}

/**
 * Parse a key specification and setup the key gen info struct
 *
 * @param ph                the plugin handle
 * @param key_spec          the key specification (ECC:CURVE or RSA:KEYBITS).
 *                          If NULL, the default key specification is used.
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _parse_key_spec(struct plugin_handle *ph, const char *key_spec,
			   struct sk_key_gen_info *gen_info)
{
	char *copy = NULL, *algorithm, *params;
	int rc = 0;

	copy = util_strdup(key_spec);

	algorithm = strtok(copy, ":");
	if (algorithm == NULL) {
		_set_error(ph, "Invalid key specification format: '%s'",
			   key_spec);
		rc = -EINVAL;
		goto out;
	}

	params = strtok(NULL, ":");
	if (params == NULL) {
		_set_error(ph, "Invalid key specification format: '%s'",
			   key_spec);
		rc = -EINVAL;
		goto out;
	}

	if (strcasecmp(algorithm, KMIP_KEY_ALGORITHM_RSA) == 0) {
		gen_info->type = SK_KEY_TYPE_RSA;
	} else if (strcasecmp(algorithm, KMIP_KEY_ALGORITHM_ECC) == 0) {
		gen_info->type = SK_KEY_TYPE_EC;
	} else {
		_set_error(ph, "Invalid key algorithm: '%s'", key_spec);
		rc = -EINVAL;
		goto out;
	}

	switch (gen_info->type) {
	case SK_KEY_TYPE_RSA:
		gen_info->rsa.modulus_bits = atol(params);
		switch (gen_info->rsa.modulus_bits) {
		case 512:
		case 1024:
		case 2048:
		case 4096:
			break;
		default:
			_set_error(ph, "Invalid RSA key bits: '%s'", key_spec);
			rc = -EINVAL;
			goto out;
		}
		gen_info->rsa.pub_exp = 65537;
		gen_info->rsa.x9_31 = false;
		break;

	case SK_KEY_TYPE_EC:
		gen_info->ec.curve_nid = OBJ_txt2nid(params);
		if (gen_info->ec.curve_nid == NID_undef) {
			_set_error(ph, "Invalid ECC curve: '%s'", key_spec);
			rc = -EINVAL;
			goto out;
		}
		break;
	}

out:
	free(copy);
	return rc;
}

/**
 * Generates (or re-generates) a identity key for the plugin using the
 * specified key specification, or the default key specifications, of none
 * is specified.
 *
 * @param ph                the plugin handle
 * @param key_spec          the key specification (ECC:CURVE or RSA:KEYBITS).
 *                          If NULL, the default key specification is used.
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _generate_identity_key(struct plugin_handle *ph,
				  const char *key_spec)
{
	unsigned char identity_key[KMIP_MAX_KEY_TOKEN_SIZE] = { 0 };
	size_t identity_key_size = sizeof(identity_key);
	char *reenc_file = NULL, *client_cert = NULL;
	struct sk_key_gen_info gen_info = { 0 };
	char tmp[200];
	int rc;

	if (key_spec == NULL)
		key_spec = KMIP_DEFAULT_IDENTITY_KEY_SPEC;

	_check_config_complete(ph);

	if (!ph->apqns_configured) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first configure the APQNs used with this plugin.");
		return -EINVAL;
	}

	rc = _parse_key_spec(ph, key_spec, &gen_info);
	if (rc != 0)
		return rc;

	if (ph->identity_secure_key != NULL) {
		printf("ATTENTION: An identity key already exists\n");
		util_print_indented("When you generate a new identity key, "
				    "you must re-generate a certificate and "
				    "re-register it with the KMIP server.", 0);
		printf("%s: Re-generate the identity key [y/N]? ",
		       program_invocation_short_name);
		if (!prompt_for_yes(ph->pd.verbose)) {
			_set_error(ph, "Operation aborted by user");
			return -ECANCELED;
		}
	} else {
		util_asprintf((char **)&ph->identity_secure_key,
			      "%s/%s", ph->pd.config_path,
			      KMIP_CONFIG_IDENTITY_KEY_FILE);

		rc = plugin_set_or_remove_property(&ph->pd,
						   KMIP_CONFIG_IDENTITY_KEY,
						   ph->identity_secure_key);
		if (rc != 0)
			goto out;
	}

	switch (gen_info.type) {
	case SK_KEY_TYPE_RSA:
		rc = plugin_set_or_remove_property(&ph->pd,
					KMIP_CONFIG_IDENTITY_KEY_ALGORITHM,
					KMIP_KEY_ALGORITHM_RSA);
		if (rc != 0)
			goto out;
		sprintf(tmp, "%lu", gen_info.rsa.modulus_bits);
		rc = plugin_set_or_remove_property(&ph->pd,
					KMIP_CONFIG_IDENTITY_KEY_PARAMS,
					tmp);
		if (rc != 0)
			goto out;
		break;

	case SK_KEY_TYPE_EC:
		rc = plugin_set_or_remove_property(&ph->pd,
					KMIP_CONFIG_IDENTITY_KEY_ALGORITHM,
					KMIP_KEY_ALGORITHM_ECC);
		if (rc != 0)
			goto out;
		rc = plugin_set_or_remove_property(&ph->pd,
					KMIP_CONFIG_IDENTITY_KEY_PARAMS,
					OBJ_nid2sn(gen_info.ec.curve_nid));
		if (rc != 0)
			goto out;
		break;
	default:
		break;
	}

	rc = _setup_ext_lib(ph);
	if (rc != 0)
		goto out;

	rc = SK_OPENSSL_generate_secure_key(identity_key, &identity_key_size,
					    &gen_info, &ph->ext_lib,
					    ph->pd.verbose);
	if (rc != 0) {
		_set_error(ph, "Failed to generate the identity key: %s",
			   strerror(-rc));
		goto out;
	}

	rc = SK_UTIL_write_key_blob(ph->identity_secure_key, identity_key,
				    identity_key_size);
	if (rc != 0) {
		_set_error(ph, "Failed to write the identity key into file "
			   "'%s': %s", ph->identity_secure_key, strerror(-rc));
		goto out;
	}

	rc = plugin_set_file_permission(&ph->pd, ph->identity_secure_key);
	if (rc != 0)
		goto out;

	reenc_file = properties_get(ph->pd.properties,
				    KMIP_CONFIG_IDENTITY_KEY_REENC);
	if (reenc_file != NULL) {
		remove(reenc_file);
		free(reenc_file);
		properties_remove(ph->pd.properties,
				  KMIP_CONFIG_IDENTITY_KEY_REENC);
	}

	client_cert = properties_get(ph->pd.properties,
				     KMIP_CONFIG_CLIENT_CERTIFICATE);
	if (client_cert != NULL) {
		remove(client_cert);
		free(client_cert);
		properties_remove(ph->pd.properties,
				  KMIP_CONFIG_CLIENT_CERTIFICATE);
		properties_remove(ph->pd.properties,
				  KMIP_CONFIG_CLIENT_CERT_ALGORITHM);
	}

	pr_verbose(&ph->pd, "Generated identity key into '%s'",
		   ph->identity_secure_key);

out:


	return rc;
}

/**
 * Add client authentication specific certificate extensions, if they are not
 * already contained. The extension list is reallocated, if required.
 * If no extended key usage extension is specified, then an 'TLS Web client
 * authentication' extension ('extendedKeyUsage=clientAuth') is added.
 * If no 'Subject Alternate Name' extension is specified, then an 'Subject
 * Alternate Name' extension with the system's host name (subjectAltName=
 * DNS:hostname) is added.
 *
 * @param ph                the plugin handle
 * @param extension_list    the list of extensions
 * @param num_extensions    the number of extensions
 * @param exts              Stack of extensions to add of NULL.
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _add_client_auth_extensions(struct plugin_handle *ph,
				       char ***extension_list,
				       size_t *num_extensions,
				       const STACK_OF(X509_EXTENSION) *exts)
{
	bool keyusage_found = false;
	bool altname_found = false;
	struct utsname utsname;
	int rc, count, k, nid;
	X509_EXTENSION *ex;
	size_t elements;
	char **list;
	size_t i;

	for (i = 0; i < *num_extensions; i++) {
		if (strncmp((*extension_list)[i], KMIP_CERT_EXT_KEY_USAGE,
			    strlen(KMIP_CERT_EXT_KEY_USAGE)) == 0)
			keyusage_found = true;
		if (strncmp((*extension_list)[i],
			    KMIP_CERT_EXT_SUBJECT_ALT_NAME,
			    strlen(KMIP_CERT_EXT_SUBJECT_ALT_NAME)) == 0)
			altname_found = true;
	}

	if (exts != NULL) {
		count = sk_X509_EXTENSION_num(exts);
		for (k = 0; k < count; k++) {
			ex = sk_X509_EXTENSION_value(exts, k);
			nid = OBJ_obj2nid(X509_EXTENSION_get_object(ex));

			switch (nid) {
			case NID_subject_alt_name:
				altname_found = true;
				break;
			case NID_ext_key_usage:
				keyusage_found = true;
				break;
			default:
				break;
			}
		}
	}

	if (keyusage_found && altname_found)
		return 0;

	elements = *num_extensions;
	if (!keyusage_found)
		elements++;

	if (!altname_found) {
		elements++;

		if (uname(&utsname) != 0) {
			rc = -errno;
			_set_error(ph, "Failed to obtain the system's "
				   "hostname: %s", strerror(-rc));
			return rc;
		}
	}

	list = util_realloc(*extension_list, elements * sizeof(char *));
	i = 0;

	if (!keyusage_found) {
		list[*num_extensions + i] =
			util_strdup(KMIP_CERT_EXT_KEY_USAGE_CLIENT_AUTH);
		i++;
	}

	if (!altname_found) {
		list[*num_extensions + i] = NULL;
		util_asprintf(&list[*num_extensions + i],
			      KMIP_CERT_EXT_SUBJECT_ALT_NAME_DNS,
			      utsname.nodename);
		i++;
	}

	*extension_list = list;
	*num_extensions = elements;
	return 0;
}

/**
 * Generates certificate signing request or self-signed certificate using the
 * identity key
 *
 * @param ph                the plugin handle
 * @param csr_pem_file      name of the PEM file to store a CSR to. NULL if no
 *                          CSR is to be generated.
 * @param sscert_pem_file   name of the PEM file to store a self-signed
 *                          certificate to. NULL if no certificate is to be
 *                          generated.
 * @param subject           the subject RNDs separated by semicolon (;). Can be
 *                          NULL if a renew certificate is specified.
 * @param extensions        the extensions separated by semicolon (;). Can be
 *                          NULL.
 * @param renew_cert_pem_file name of a PEM file containing a certificate to
 *                          renew. Can be NULL.
 * @param csr_new_header    if true output NEW header and footer lines in CSR
 * @param validity_days     the number of days the certificate is valid. Only
 *                          valid when generating a self-signed certificate.
 *                          Can be NULL.
 * @param digest            the digest to use with CSR and certificates. Can be
 *                          NULL
 * @param rsa_pss           if true, RSA-PSS is used with RSA-based identity
 *                          keys
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _generate_csr_sscert(struct plugin_handle *ph,
				const char *csr_pem_file,
				const char *sscert_pem_file,
				const char *subject, const char *extensions,
				const char *renew_cert_pem_file,
				bool csr_new_header, const char *validity_days,
				const char *digest, bool rsa_pss)
{
	struct sk_rsa_pss_params rsa_pss_parms = {
		.salt_len = RSA_PSS_SALTLEN_DIGEST, .mgf_digest_nid = 0 };
	unsigned char identity_key[KMIP_MAX_KEY_TOKEN_SIZE] = { 0 };
	size_t identity_key_size = sizeof(identity_key);
	char **subject_rdn_list = NULL;
	char **extension_list = NULL;
	size_t num_subject_rdns = 0;
	int digest_nid = NID_undef;
	size_t num_extensions = 0;
	char *client_cert = NULL;
	X509 *renew_cert = NULL;
	const char *cert_algo;
	X509_REQ *csr = NULL;
	X509 *ss_cert = NULL;
	int days = 30;
	int rc = 0;
	size_t i;

	_check_config_complete(ph);

	if (!ph->apqns_configured) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first configure the APQNs used with this plugin.");
		return -EINVAL;
	}
	if (!ph->identity_key_generated) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first generate the identity key.");
		return -EINVAL;
	}

	if (csr_pem_file != NULL && sscert_pem_file != NULL) {
		_set_error(ph, "Either '--gen-csr' or option "
			   "'--gen-self-signed-cert' can be specified.");
		return -EINVAL;
	}
	if (csr_new_header && csr_pem_file == NULL) {
		_set_error(ph, "Option '--csr-new-header' is only valid with "
			   "option '--gen-csr'.");
		return -EINVAL;
	}
	if (validity_days != NULL && sscert_pem_file == NULL) {
		_set_error(ph, "Option '--cert-validity-days' is only valid "
			   "with option '--gen-self-signed-cert'.");
		return -EINVAL;
	}
	if (subject == NULL && renew_cert_pem_file == NULL) {
		_set_error(ph, "Option '--cert-subject' is required, unless "
			   " option '--renew-cert' is specified.");
		return -EINVAL;
	}

	if (sscert_pem_file != NULL && ph->client_cert_avail) {
		printf("ATTENTION: A client certificate already exists\n");
		util_print_indented("When you generate a new client "
				    "certificate, the existing certificate is "
				    "removed and must re-register the newly "
				    "created certificate with the KMIP server "
				    "and the KMIP plugin before you can "
				    "communicate with the KMIP server", 0);
		printf("%s: Re-generate the client certificate [y/N]? ",
		       program_invocation_short_name);
		if (!prompt_for_yes(ph->pd.verbose)) {
			_set_error(ph, "Operation aborted by user");
			return -ECANCELED;
		}
	}

	if (validity_days != NULL) {
		days = atoi(validity_days);
		if (days <= 0) {
			_set_error(ph, "Invalid validity days: '%s'",
				   validity_days);
			return -EINVAL;
		}
	}

	if (digest != NULL) {
		digest_nid = OBJ_txt2nid(digest);
		if (digest_nid == NID_undef) {
			_set_error(ph, "Invalid digest: '%s'", digest);
			return -EINVAL;
		}
	}

	if (subject != NULL) {
		rc = parse_list(subject, &subject_rdn_list, &num_subject_rdns);
		if (rc != 0)
			goto out;
	}

	if (extensions != NULL) {
		rc = parse_list(extensions, &extension_list, &num_extensions);
		if (rc != 0)
			goto out;
	}

	if (renew_cert_pem_file != NULL) {
		rc = SK_UTIL_read_x509_certificate(renew_cert_pem_file,
						   &renew_cert);
		if (rc != 0) {
			_set_error(ph, "Failed to load the renew certificate "
				   "from'%s'", renew_cert_pem_file);
			goto out;
		}
	}

	rc = _add_client_auth_extensions(ph, &extension_list, &num_extensions,
					 renew_cert != NULL ?
						X509_get0_extensions(renew_cert)
						: NULL);
	if (rc != 0)
		goto out;

	rc = _setup_ext_lib(ph);
	if (rc != 0)
		goto out;

	rc = SK_UTIL_read_key_blob(ph->identity_secure_key, identity_key,
				   &identity_key_size);
	if (rc != 0) {
		_set_error(ph, "Failed to load the identity key from '%s': %s",
			   ph->identity_secure_key, strerror(-rc));
		goto out;
	}

	if (csr_pem_file != NULL) {
		rc = SK_OPENSSL_generate_csr(identity_key, identity_key_size,
					     (const char **)subject_rdn_list,
					     num_subject_rdns, true, renew_cert,
					     (const char **)extension_list,
					     num_extensions, digest_nid,
					     rsa_pss ? &rsa_pss_parms : NULL,
					     &csr, &ph->ext_lib, ph->pd.verbose);
	} else {
		rc = SK_OPENSSL_generate_ss_cert(identity_key,
						 identity_key_size,
						(const char **)subject_rdn_list,
						 num_subject_rdns, true,
						 renew_cert,
						 (const char **)extension_list,
						 num_extensions, days,
						 digest_nid,
						 rsa_pss ? &rsa_pss_parms :
									NULL,
						 &ss_cert, &ph->ext_lib,
						 ph->pd.verbose);
	}
	switch (rc) {
	case 0:
		break;
	case -EBADMSG:
		_set_error(ph, "The subject or extensions could not be parsed "
			   "or are not recognized by OpenSSL.");
		rc = -EINVAL;
		goto out;
	case -EEXIST:
		_set_error(ph, "One of the subject name entries or extensions "
			   "is a duplicate.");
		rc = -EINVAL;
		goto out;
	case -ENOTSUP:
		_set_error(ph, "The specified digest is not supported.");
		rc = -EINVAL;
		goto out;
	default:
		_set_error(ph, "Failed to generate the %s: %s",
			   csr_pem_file != NULL ? "certificate signing request"
					   : "self-signed certificate",
			   strerror(-rc));
		goto out;
	}

	if (csr_pem_file != NULL) {
		rc = SK_UTIL_write_x509_request(csr_pem_file, csr,
						csr_new_header);
		if (rc != 0) {
			_set_error(ph, "Failed to write the certificate "
				   "signing request to '%s'", csr_pem_file);
			goto out;
		}

		pr_verbose(&ph->pd, "Generated certificate signing request "
			   "into '%s'", csr_pem_file);
	} else {
		switch (EVP_PKEY_id(X509_get0_pubkey(ss_cert))) {
		case EVP_PKEY_RSA:
			cert_algo = KMIP_KEY_ALGORITHM_RSA;
			break;
		case EVP_PKEY_RSA_PSS:
			cert_algo = KMIP_KEY_ALGORITHM_RSA_PSS;
			rsa_pss = true;
			break;
		case EVP_PKEY_EC:
			cert_algo = KMIP_KEY_ALGORITHM_ECC;
			break;
		default:
			_set_error(ph, "Unsupported certificate algorithm");
			rc = -EINVAL;
			goto out;
		}

		rc = SK_UTIL_write_x509_certificate(sscert_pem_file, ss_cert);
		if (rc != 0) {
			_set_error(ph, "Failed to write the self-signed "
				   "certificate to '%s'", sscert_pem_file);
			goto out;
		}

		util_asprintf(&client_cert, "%s/%s", ph->pd.config_path,
			      KMIP_CONFIG_CLIENT_CERTIFICATE_FILE);

		rc = plugin_set_or_remove_property(&ph->pd,
				KMIP_CONFIG_CLIENT_CERTIFICATE, client_cert);
		if (rc != 0)
			goto out;

		rc = plugin_set_or_remove_property(&ph->pd,
				KMIP_CONFIG_CLIENT_CERT_ALGORITHM, cert_algo);
		if (rc != 0)
			goto out;

		rc = SK_UTIL_write_x509_certificate(client_cert, ss_cert);
		if (rc != 0) {
			_set_error(ph, "Failed to write the self-signed "
				   "certificate to '%s'", client_cert);
			goto out;
		}

		pr_verbose(&ph->pd, "Generated self-signed certificate into "
			   "'%s' and '%s'", sscert_pem_file, client_cert);
	}

out:
	if (subject_rdn_list != NULL) {
		for (i = 0; i < num_subject_rdns; i++)
			free(subject_rdn_list[i]);
		free(subject_rdn_list);
	}
	if (extension_list != NULL) {
		for (i = 0; i < num_extensions; i++)
			free(extension_list[i]);
		free(extension_list);
	}
	if (renew_cert != NULL)
		X509_free(renew_cert);
	if (ss_cert != NULL)
		X509_free(ss_cert);
	if (csr != NULL)
		X509_REQ_free(csr);
	if (client_cert != NULL)
		free(client_cert);

	return rc;
}

/**
 * Checks that none of the options for generating a CSR or self-signed
 * certificate is specified, and sets up the error message and return code if
 * so.
 *
 * @param ph                the plugin handle
 * @param opts              the config options structure
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _error_gen_csr_sscert_opts(struct plugin_handle *ph,
				      struct config_options *opts)
{
	int rc = 0;

	if (opts->cert_subject != NULL) {
		_set_error(ph, "Option '--cert-subject' is only valid "
			   "together with options '--gen-csr' or "
			   "'--gen-self-signed-cert'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->cert_extensions != NULL) {
		_set_error(ph, "Option '--cert-extensions' is only "
			   "valid together with options '--gen-csr' or "
			   "'--gen-self-signed-cert'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->renew_cert_pem_file != NULL) {
		_set_error(ph, "Option '--renew-cert' is only "
			   "valid together with options '--gen-csr' or "
			   "'--gen-self-signed-cert'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->csr_new_header == true) {
		_set_error(ph, "Option '--csr-new-header' is only "
			   "valid together with option '--gen-csr'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->cert_validity_days != NULL) {
		_set_error(ph, "Option '--cert-validity-days' is only "
			   "valid together with option "
			   "'--gen-self-signed-cert'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->cert_digest != NULL) {
		_set_error(ph, "Option '--cert-digest' is only "
			   "valid together with options '--gen-csr' or "
			   "'--gen-self-signed-cert'.");
		rc = -EINVAL;
		goto out;
	}
	if (opts->cert_rsa_pss == true) {
		_set_error(ph, "Option '--cert-rsa-pss' is only "
			   "valid together with option '--gen-csr' or "
			   "'--gen-self-signed-cert'");
		rc = -EINVAL;
		goto out;
	}

out:
	return rc;
}

/**
 * Use a client certificate with the KMIP plugin. The client certificate's
 * public key must match the identity key.
 *
 * @param ph                the plugin handle
 * @param client_cert       The client certificate to use
 *
 * @returns 0 on success, a negative errno in case of an error.
 */
static int _use_client_cert(struct plugin_handle *ph, const char *client_cert)
{
	unsigned char identity_key[KMIP_MAX_KEY_TOKEN_SIZE] = { 0 };
	size_t identity_key_size = sizeof(identity_key);
	char *client_cert_file = NULL;
	EVP_PKEY *pkey = NULL;
	bool rsa_pss = false;
	X509 *cert = NULL;
	char *cert_algo;
	int rc;

	_check_config_complete(ph);

	if (!ph->apqns_configured) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first configure the APQNs used with this plugin.");
		return -EINVAL;
	}
	if (!ph->identity_key_generated) {
		_set_error(ph, "The configuration is incomplete, you must "
			   "first generate the identity key.");
		return -EINVAL;
	}

	if (ph->client_cert_avail) {
		printf("ATTENTION: A client certificate already exists\n");
		util_print_indented("When you set a new client certificate, "
				    "the existing certificate is removed and "
				    "you must re-register the new certificate "
				    "with the KMIP server before you can "
				    "communicate with the KMIP server", 0);
		printf("%s: Set the new client certificate [y/N]? ",
		       program_invocation_short_name);
		if (!prompt_for_yes(ph->pd.verbose)) {
			_set_error(ph, "Operation aborted by user");
			return -ECANCELED;
		}
	}

	rc = _setup_ext_lib(ph);
	if (rc != 0)
		goto out;

	rc = SK_UTIL_read_x509_certificate(client_cert, &cert);
	if (rc != 0) {
		_set_error(ph, "Failed to read the client certificate from "
			   "file '%s': %s", client_cert, strerror(-rc));
		return rc;
	}

	if (ph->pd.verbose) {
		pr_verbose(&ph->pd, "Client certificate read from '%s'",
			   client_cert);
		X509_print_fp(stderr, cert);
	}

	rc = SK_UTIL_read_key_blob(ph->identity_secure_key, identity_key,
				   &identity_key_size);
	if (rc != 0) {
		_set_error(ph, "Failed to load the identity key from '%s': %s",
			   ph->identity_secure_key, strerror(-rc));
		goto out;
	}

	switch (EVP_PKEY_id(X509_get0_pubkey(cert))) {
	case EVP_PKEY_RSA:
		cert_algo = KMIP_KEY_ALGORITHM_RSA;
		break;
	case EVP_PKEY_RSA_PSS:
		cert_algo = KMIP_KEY_ALGORITHM_RSA_PSS;
		rsa_pss = true;
		break;
	case EVP_PKEY_EC:
		cert_algo = KMIP_KEY_ALGORITHM_ECC;
		break;
	default:
		_set_error(ph, "Unsupported certificate algorithm");
		rc = -EINVAL;
		goto out;
	}

	rc = SK_OPENSSL_get_secure_key_as_pkey(identity_key, identity_key_size,
					       rsa_pss, &pkey, &ph->ext_lib,
					       ph->pd.verbose);
	if (rc != 0) {
		_set_error(ph, "Failed to get the PKEY from the identity key: "
			   "%s", strerror(-rc));
		goto out;
	}

#if !OPENSSL_VERSION_PREREQ(3, 0)
	if (EVP_PKEY_cmp(X509_get0_pubkey(cert), pkey) != 1) {
#else
	if (EVP_PKEY_eq(X509_get0_pubkey(cert), pkey) != 1) {
#endif
		_set_error(ph, "The client certificate's public key does not "
			   "match the identity key.");
		rc = -EINVAL;
		goto out;
	}

	util_asprintf(&client_cert_file, "%s/%s", ph->pd.config_path,
		      KMIP_CONFIG_CLIENT_CERTIFICATE_FILE);

	rc = plugin_set_or_remove_property(&ph->pd,
			KMIP_CONFIG_CLIENT_CERTIFICATE, client_cert_file);
	if (rc != 0)
		goto out;

	rc = plugin_set_or_remove_property(&ph->pd,
			KMIP_CONFIG_CLIENT_CERT_ALGORITHM, cert_algo);
	if (rc != 0)
		goto out;

	rc = SK_UTIL_write_x509_certificate(client_cert_file, cert);
	if (rc != 0) {
		_set_error(ph, "Failed to write the self-signed "
			   "certificate to '%s'", client_cert_file);
		goto out;
	}

	pr_verbose(&ph->pd, "Client certificate stored in '%s'",
		   client_cert_file);

out:
	if (pkey != NULL)
		EVP_PKEY_free(pkey);
	if (client_cert_file != NULL)
		free(client_cert_file);
	X509_free(cert);

	return rc;
}

/**
 * Configures (or re-configures) a KMS plugin. This function can be called
 * several times to configure a KMS plugin is several steps (if supported by the
 * KMS plugin). In case a configuration is not fully complete, this function
 * may return -EAGAIN to indicate that it has accepted the configuration so far,
 * but the configuration needs to be completed.
 *
 * A KMS plugin must be associated with at least one APQN. Thus, in a multi-step
 * configuration, a list f APQNs must be specified at least once.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param apqns             a list of APQNs to associate with the KMS plugin, or
 *                          NULL if no APQNs are specified.
 * @param num_apqns         number of APQNs in above array. 0 if no APQNs are
 *                          specified.
 * @param options           a list of options as specified by the user. These
 *                          options are a subset of the possible options as
 *                          returned by kms_get_command_options() with command
 *                          KMS_COMMAND_CONFIGURE.
 * @param num_options       number of options in above array.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 * -EAGAIN to indicate that the specified configuration was accepted so far, but
 * the configuration is still incomplete, and needs to be completed.
 */
int kms_configure(const kms_handle_t handle,
		  const struct kms_apqn *apqns, size_t num_apqns,
		  const struct kms_option *options, size_t num_options)
{
	struct config_options opts = { 0 };
	struct plugin_handle *ph = handle;
	bool config_changed = false;
	char *apqn_str = NULL;
	int rc = 0;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(num_apqns == 0 || apqns != NULL,
		    "Internal error: apqns is NULL  but num_apqns > 0");
	util_assert(num_options == 0 || options != NULL,
		    "Internal error: options is NULL but num_options > 0 ");

	pr_verbose(&ph->pd, "Configure");
	for (i = 0; i < num_apqns; i++) {
		pr_verbose(&ph->pd, "  APQN: %02x.%04x", apqns[i].card,
			   apqns[i].domain);
	}
	for (i = 0; i < num_options; i++) {
		if (isalnum(options[i].option))
			pr_verbose(&ph->pd, "  Option '%c': '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
		else
			pr_verbose(&ph->pd, "  Option %d: '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
	}

	plugin_clear_error(&ph->pd);

	if (apqns != NULL) {
		rc = _check_apqns(ph, apqns, num_apqns);
		if (rc != 0)
			goto out;

		if (num_apqns > 0 && ph->card_type == CARD_TYPE_CCA) {
			rc = cross_check_cca_apka_apqns(&ph->pd, apqns,
							num_apqns);
			if (rc != 0) {
				_set_error(ph, "Your CCA APKA master key setup "
					   "is improper");
				goto out;
			}
		}

		apqn_str = build_kms_apqn_string(apqns, num_apqns);
		rc = properties_set(ph->pd.properties, KMIP_CONFIG_APQNS,
				    apqn_str);
		if (rc != 0) {
			_set_error(ph, "Failed to set APQNs property: %s",
				   strerror(-rc));
			goto out;
		}

		rc = properties_set(ph->pd.properties, KMIP_CONFIG_APQN_TYPE,
				    _card_type_to_str(ph->card_type));
		if (rc != 0) {
			_set_error(ph, "Failed to set APQN-Type property: %s",
				   strerror(-rc));
			goto out;
		}

		config_changed = true;
	}

	for (i = 0; i < num_options; i++) {
		switch (options[i].option) {
		case 'i':
			opts.generate_identity_key = options[i].argument;
			break;
		case 'c':
			opts.csr_pem_file = options[i].argument;
			break;
		case 'C':
			opts.sscert_pem_file = options[i].argument;
			break;
		case 's':
			opts.cert_subject = options[i].argument;
			break;
		case 'e':
			opts.cert_extensions = options[i].argument;
			break;
		case 'N':
			opts.renew_cert_pem_file = options[i].argument;
			break;
		case 'n':
			opts.csr_new_header = true;
			break;
		case 'd':
			opts.cert_validity_days = options[i].argument;
			break;
		case 'D':
			opts.cert_digest = options[i].argument;
			break;
		case 'P':
			opts.cert_rsa_pss = true;
			break;
		case 'r':
			opts.client_cert = options[i].argument;
			break;
		default:
			rc = -EINVAL;
			if (isalnum(options[i].option))
				_set_error(ph, "Unsupported option '%c'",
					   options[i].option);
			else
				_set_error(ph, "Unsupported option %d",
					   options[i].option);
			goto out;
		}
	}

	if (opts.generate_identity_key != NULL) {
		rc = _generate_identity_key(ph, opts.generate_identity_key);
		if (rc != 0)
			goto out;

		config_changed = true;
	}

	if (opts.csr_pem_file != NULL || opts.sscert_pem_file != NULL) {
		if (opts.client_cert != NULL) {
			_set_error(ph, "Option '--client-cert' in not valid "
				   "together with options '--gen-csr' or "
				   "'--gen-self-signed-cert'.");
			rc = -EINVAL;
			goto out;
		}

		if (!ph->identity_key_generated) {
			/* Generate identity key with default key-specs */
			rc = _generate_identity_key(ph, NULL);
			if (rc != 0)
				goto out;

			config_changed = true;
		}

		rc = _generate_csr_sscert(ph, opts.csr_pem_file,
					  opts.sscert_pem_file,
					  opts.cert_subject,
					  opts.cert_extensions,
					  opts.renew_cert_pem_file,
					  opts.csr_new_header,
					  opts.cert_validity_days,
					  opts.cert_digest,
					  opts.cert_rsa_pss);
		config_changed = true;
	} else {
		rc = _error_gen_csr_sscert_opts(ph, &opts);
	}
	if (rc != 0)
		goto out;

	if (opts.client_cert != NULL) {
		rc = _use_client_cert(ph, opts.client_cert);
		if (rc != 0)
			goto out;

		config_changed = true;
	}

out:
	if (apqn_str != NULL)
		free(apqn_str);

	if (rc == 0) {
		if (config_changed) {
			rc = plugin_save_config(&ph->pd);
			if (rc != 0)
				goto ret;

			_check_config_complete(ph);
			pr_verbose(&ph->pd,
				   "Plugin configuration is %scomplete",
				   ph->config_complete ? "" : "in");
		}

		if (!ph->config_complete)
			rc = -EAGAIN;
	}

ret:
	return rc;
}

/**
 * De-configures a KMS plugin. This is called by zkey when a repository is
 * unbound from a KMS plugin. It gives the KMS plugin the chance to gracefully
 * remove any files that the plugin has stored in its config directory. zkey
 * will unconditionally remove all left over files when this function returns.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_deconfigure(const kms_handle_t handle)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");

	pr_verbose(&ph->pd, "Deconfigure");

	plugin_clear_error(&ph->pd);

	return 0;
}

/**
 * Allows the KMS plugin to perform a login to the KMS (if required). This
 * function is called at least once before any key operation function, typically
 * shortly after opening the repository.
 * The KMS plugin may prompt the user (by reading from stdin) for its
 * credentials, if needed.
 *
 * It is suggested that a KMS plugin performs a login with the KMS once, and
 * stores a login token (or similar) in its config directory. The next time
 * the kms_login function is called, the login token can be reused (if still
 * valid). This avoids to prompt the user for every key operation.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_login(const kms_handle_t handle)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");

	pr_verbose(&ph->pd, "Login");

	plugin_clear_error(&ph->pd);

	return 0;
}

/**
 * Called when the master keys of an APQN associated with the KMS plugin has
 * been changed. The KMS plugin can then re-encipher all its secure keys (if
 * any) that it has stored in its config directory.
 *
 * Keys that have been generated by the KMS plugin and stored in the zkey
 * repository do not need to be re-enciphered by the KMS plugin. Those are
 * re-enciphered by zkey without the help of the KMS plugin.
 *
 * HSM have different master key registers. Typically a CURRENT and a NEW master
 * key register exists. The NEW register may be loaded with the new to be set
 * master key, and secure keys can be re-enciphered with it proactively.
 *
 * CCA also supports an OLD master key register, that contains the previously
 * used master key. You thus can re-encipher a secure key that is currently
 * enciphered with the master key from the OLD register with the master key
 * from the CURRENT register.
 *
 * HSMs may also support different master keys for different key types or
 * algorithms. It is up to the KMS plugin to know which master key registers
 * are used for its secure keys
 *
 * A staged re-encipherment is performed by re-enciphering a secure key with
 * the new HSM master key, without making it available for use in the first
 * stage. Only when the staged re-encipherment is completed, then the previously
 * re-enciphered secure key is make available for use and the old on is removed.
 *
 * An in-place re-encipherment replaces the secure key right away with its
 * re-enciphered version.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param mode              Re-encipherment mode
 * @param mkreg             Re-encipherment register selection
 * @param options           a list of options as specified by the user. These
 *                          options are a subset of the possible options as
 *                          returned by kms_get_command_options() with command
 *                          KMS_COMMAND_REENCIPHER.
 * @param num_options       number of options in above array.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_reenciper(const kms_handle_t handle, enum kms_reencipher_mode mode,
		  enum kms_reenc_mkreg mkreg,
		  const struct kms_option *options, size_t num_options)
{
	struct plugin_handle *ph = handle;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(num_options == 0 || options != NULL,
		    "Internal error: options is NULL but num_options > 0 ");

	pr_verbose(&ph->pd, "Re-encipher mode: %d, kmreg=%d", mode, mkreg);
	for (i = 0; i < num_options; i++) {
		if (isalnum(options[i].option))
			pr_verbose(&ph->pd, "  Option '%c': '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
		else
			pr_verbose(&ph->pd, "  Option %d: '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
	}

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * Generates a key in or with the KMS and returns a secure key that is
 * enciphered under the current HSM master key.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_type          the zkey key type, euch as 'CCA-AESDATA',
 *                          'CCA-AESCIPHER', 'EP11-AES'.
 * @param key_bits          the key bit size (e.g. 256 for an AES 256 bit key).
 * @param properties        a list of properties to associate the key with
 * @param num_properties    the number of properties in above array
 * @param options           a list of options as specified by the user. These
 *                          options are a subset of the possible options as
 *                          returned by kms_get_command_options() with command
 *                          KMS_COMMAND_GENERATE.
 * @param num_options       number of options in above array.
 * @param key_blob          a buffer to return the key blob. The size of the
 *                          buffer is specified in key_blob_length
 * @param key_blob_length   on entry: the size of the key_blob buffer.
 *                          on exit: the size of the key blob returned.
 * @param key_id            a buffer to return the key-ID of the generated key.
 *                          The key-id is a textual identifier uniquely
 *                          identifying a key in the KMS and the KMS plugin.
 *                          The returned key-id contains the terminating zero.
 * @paran key_id_size       size of the key_id buffer. It should be at least
 *                          KMS_KEY_ID_SIZE + 1 bytes large.
 * @param key_label         a buffer to return the key-label of the generated
 *                          key. The key-label is a textual identifier used to
 *                          identify a key in the user interface of the KMS.
 *                          A key label may be equal to the key-ID, or it may
 *                          different. The returned key-label contains the
 *                          terminating zero.
 * @paran key_label_size    size of the key_lanble buffer. It should be at least
 *                          KMS_KEY_LABEL_SIZE + 1 bytes large.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_generate_key(const kms_handle_t handle, const char *key_type,
		     size_t key_bits, enum kms_key_mode key_mode,
		     const struct kms_property *properties,
		     size_t num_properties,
		     const struct kms_option *options, size_t num_options,
		     unsigned char *key_blob, size_t *key_blob_length,
		     char *key_id, size_t key_id_size,
		     char *key_label, size_t key_label_size)
{
	struct plugin_handle *ph = handle;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(num_properties == 0 || properties != NULL,
		    "Internal error: properties is NULL but num_properties"
		    " > 0 ");
	util_assert(num_options == 0 || options != NULL,
		    "Internal error: options is NULL but num_options > 0 ");
	util_assert(key_blob != NULL, "Internal error: key_blob is NULL");
	util_assert(key_blob_length != NULL, "Internal error: key_blob_length "
		    "is NULL");
	util_assert(key_id != NULL, "Internal error: key_id is NULL");
	util_assert(key_label != NULL, "Internal error: key_label is NULL");

	pr_verbose(&ph->pd, "Generate key: key-type: '%s', keybits: %lu, "
		   "mode: %d", key_type, key_bits, key_mode);
	for (i = 0; i < num_properties; i++) {
		util_assert(properties[i].name != NULL,
			    "Internal error: property name is NULL");
		util_assert(properties[i].value != NULL,
			    "Internal error: property value is NULL");
		pr_verbose(&ph->pd, "  Property '%s': '%s'", properties[i].name,
			   properties[i].value);
	}
	for (i = 0; i < num_options; i++) {
		if (isalnum(options[i].option))
			pr_verbose(&ph->pd, "  Option '%c': '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
		else
			pr_verbose(&ph->pd, "  Option %d: '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
	}

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * Sets (adds/replaces/removes) properties of a key. Already existing properties
 * with the same property name are replaced, non-existing properties are added.
 * To remove a property, set the property value to NULL.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_id            the key-ID to set the properties for
 * @param properties        a list of properties to set
 * @param num_properties    the number of properties in above array
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_set_key_properties(const kms_handle_t handle, const char *key_id,
			   const struct kms_property *properties,
			   size_t num_properties)
{
	struct plugin_handle *ph = handle;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(key_id != NULL, "Internal error: key_id is NULL");
	util_assert(num_properties == 0 || properties != NULL,
		    "Internal error: properties is NULL but num_properties"
		    " > 0 ");

	pr_verbose(&ph->pd, "Set key properties: key-ID: '%s'", key_id);
	for (i = 0; i < num_properties; i++) {
		util_assert(properties[i].name != NULL,
			    "Internal error: property name is NULL");

		pr_verbose(&ph->pd, "  Property '%s': '%s'", properties[i].name,
			   properties[i].value != NULL ? properties[i].value :
			   "(null)");
	}

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * Gets properties of a key.
 *
 * The returned list of properties must be freed by the caller. Each property
 * name and value must be freed individually (using free()), as well as the
 * complete array.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_id            the key-ID to set the properties for
 * @param properties        On return: a list of properties
 * @param num_properties    On return: the number of properties in above array
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_get_key_properties(const kms_handle_t handle, const char *key_id,
			   struct kms_property **properties,
			   size_t *num_properties)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(key_id != NULL, "Internal error: key_id is NULL");
	util_assert(properties != NULL, "Internal error: properties is NULL");
	util_assert(num_properties != NULL,
		    "Internal error: num_properties is NULL");

	pr_verbose(&ph->pd, "Get key properties: key-ID: '%s'", key_id);

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * Called when zkey removes a KMS-bound key from the zkey repository. The KMS
 * plugin can then set the state of the key in the KMS, or remove it also from
 * the KMS (this is usually not done).
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_id            the key-ID to set the properties for
 * @param options           a list of options as specified by the user. These
 *                          options are a subset of the possible options as
 *                          returned by kms_get_command_options() with command
 *                          KMS_COMMAND_REMOVE.
 * @param num_options       number of options in above array.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_remove_key(const kms_handle_t handle, const char *key_id,
		   const struct kms_option *options, size_t num_options)
{
	struct plugin_handle *ph = handle;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(key_id != NULL, "Internal error: key_id is NULL");
	util_assert(num_options == 0 || options != NULL,
		    "Internal error: options is NULL but num_options > 0 ");

	pr_verbose(&ph->pd, "Remove key: key-ID: '%s'", key_id);
	for (i = 0; i < num_options; i++) {
		if (isalnum(options[i].option))
			pr_verbose(&ph->pd, "  Option '%c': '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
		else
			pr_verbose(&ph->pd, "  Option %d: '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
	}

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * List keys managed by the KMS. This list is independent of the zkey key
 * repository. It lists keys as known by the KMS.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param label_pattern     a pattern of the label used to filter the keys, or
 *                          NULL if no label pattern is specified.
 * @param properties        a list of properties used to filter the keys, or
 *                          NULL if no properties filter is specified.
 * @param num_properties    the number of properties in above array.
 * @param options           a list of options as specified by the user. These
 *                          options are a subset of the possible options as
 *                          returned by kms_get_command_options() with command
 *                          KMS_COMMAND_LIST.
 * @param num_options       number of options in above array.*
 * @param callback          a callback function that is called for each key that
 *                          matches the filter (if any).
 * @private_data            a private pointer passed as is to the callback
 *                          function. Can be used to pass user specific
 *                          information to the callback.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_list_keys(const kms_handle_t handle, const char *label_pattern,
		  const struct kms_property *properties, size_t num_properties,
		  const struct kms_option *options, size_t num_options,
		  kms_list_callback callback, void *private_data)
{
	struct plugin_handle *ph = handle;
	size_t i;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(num_properties == 0 || properties != NULL,
		    "Internal error: properties is NULL but num_properties "
		    "> 0 ");
	util_assert(callback != NULL, "Internal error: callback is NULL");

	pr_verbose(&ph->pd, "List Keys, label-pattern: '%s'",
		   label_pattern != NULL ? label_pattern : "(null)");
	for (i = 0; i < num_properties; i++) {
		util_assert(properties[i].name != NULL,
			    "Internal error: property name is NULL");
		util_assert(properties[i].value != NULL,
			    "Internal error: property value is NULL");
		pr_verbose(&ph->pd, "  Property '%s': '%s'", properties[i].name,
			   properties[i].value);
	}
	for (i = 0; i < num_options; i++) {
		if (isalnum(options[i].option))
			pr_verbose(&ph->pd, "  Option '%c': '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
		else
			pr_verbose(&ph->pd, "  Option %d: '%s'",
				   options[i].option,
				   options[i].argument != NULL ?
					options[i].argument : "(null)");
	}

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

/**
 * Imports a key from the KMS and returns a secure key that is
 * enciphered under the current HSM master key.
 *
 * @param handle            the KMS plugin handle obtained from kms_initialize()
 * @param key_id            the key-ID of the key to import
 * @param key_type          the zkey key type, like 'CCA-AESDATA',
 *                          'CCA-AESCIPHER', 'EP11-AES'.
 * @param key_blob          a buffer to return the key blob. The size of the
 *                          buffer is specified in key_blob_length
 * @param key_blob_length   on entry: the size of the key_blob buffer.
 *                          on exit: the size of the key blob returned.
 *
 * @returns 0 on success, or a negative errno in case of an error.
 * Function kms_get_last_error() can be used to obtain more details about the
 * error.
 */
int kms_import_key2(const kms_handle_t handle, const char *key_id,
		    const char *key_type,
		    unsigned char *key_blob, size_t *key_blob_length)
{
	struct plugin_handle *ph = handle;

	util_assert(handle != NULL, "Internal error: handle is NULL");
	util_assert(key_blob != NULL, "Internal error: key_blob is NULL");
	util_assert(key_blob_length != NULL, "Internal error: key_blob_length "
		    "is NULL");

	pr_verbose(&ph->pd, "Import Key, key-ID: '%s'", key_id);

	plugin_clear_error(&ph->pd);

	return -ENOTSUP;
}

static const struct kms_functions kms_functions = {
	.api_version = KMS_API_VERSION_2,
	.kms_bind = kms_bind,
	.kms_initialize = kms_initialize,
	.kms_terminate = kms_terminate,
	.kms_get_last_error = kms_get_last_error,
	.kms_supports_key_type = kms_supports_key_type,
	.kms_display_info = kms_display_info,
	.kms_get_command_options = kms_get_command_options,
	.kms_configure = kms_configure,
	.kms_deconfigure = kms_deconfigure,
	.kms_login = kms_login,
	.kms_reenciper = kms_reenciper,
	.kms_generate_key = kms_generate_key,
	.kms_set_key_properties = kms_set_key_properties,
	.kms_get_key_properties = kms_get_key_properties,
	.kms_remove_key = kms_remove_key,
	.kms_list_keys = kms_list_keys,
	.kms_import_key2 = kms_import_key2,
};

/**
 * Returns an address of a structure containing the KMS plugin functions.
 * This function is exported by the KMS plugin, and its address is obtain
 * via dlsym() after loading the plugin via dlopen().
 * *
 * @returns the address of a structure or NULL in case of an error.
 */
const struct kms_functions *kms_get_functions(void)
{
	return &kms_functions;
}