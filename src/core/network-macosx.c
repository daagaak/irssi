/*
 network-macosx.c : Secure Transport support
 
 Copyright (C) 2011 Matt Wright
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <Security/Security.h>

#include "module.h"
#include "network.h"
#include "misc.h"

/* declare our escape into MacIrssi land */
int irssibridge_present_trust_panel(SecTrustRef trust);

/* ssl i/o channel object */
typedef struct
{
	GIOChannel pad;
	gint fd;
	GIOChannel *giochan;
  SSLContextRef context;
	unsigned int verify:1;
	const char *hostname;
} GIOSSLChannel;

SecIdentityRef CreateIdentityFromCommonName(const char *certificate, const char *privateKey)
{
  /* due to keychain constraints, only keychain identities are usable */
  SecIdentitySearchRef identitySearchRef = nil;
  
  OSStatus status = SecIdentitySearchCreate(NULL, 0, &identitySearchRef);
  if (status != noErr) {
    g_warning("Unable to create search for keychain identity matching \"%s\"", certificate);
    return NULL;
  }
  
  CFStringRef certificateCommonNameRef = CFStringCreateWithCString(kCFAllocatorDefault, certificate, kCFStringEncodingUTF8);
  SecIdentityRef discoveredIdentityRef = nil;
  
  SecIdentityRef identityRef = nil;
  while ((status = SecIdentitySearchCopyNext(identitySearchRef, &identityRef)) == noErr) {
    /* get the certificate for each and see if it matches certificate */
    SecCertificateRef certificateRef;
    
    OSStatus substatus = SecIdentityCopyCertificate(identityRef, &certificateRef);
    if (substatus != noErr) {
      continue;
    }
    
    CFStringRef commonNameRef;
    substatus = SecCertificateCopyCommonName(certificateRef, &commonNameRef);
    if (substatus != noErr) {
      CFRelease(certificateRef);
      continue;
    }
    
    if (CFStringCompare(commonNameRef, certificateCommonNameRef, 0) == kCFCompareEqualTo) {
      discoveredIdentityRef = identityRef;
      CFRelease(commonNameRef);
      CFRelease(certificateRef);
      /* itentionally not releasing identity here */
      break;
    }
    CFRelease(commonNameRef);
    CFRelease(certificateRef);
    CFRelease(identityRef);
  }
  
  CFRelease(certificateCommonNameRef);
  CFRelease(identitySearchRef);
  
  if (!discoveredIdentityRef) {
    g_warning("Unable to find keychain identity with common name \"%s\".", certificate);
    return NULL;
  }
  
  /* return found identity with create rule */
  return identityRef;
}

OSStatus _SSLReadFunction(SSLConnectionRef connection, void *data, size_t *dataLength)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)connection;
  
  size_t len = 0;
  while (len < *dataLength) {
    size_t res = read(channel->fd, ((char*)data)+len, (*dataLength)-len);
    
    if (res == -1) {
      if (errno == EAGAIN) {
        /* eject if we get EAGAIN, means we're in non-block mode */
        *dataLength = len;
        return errSSLWouldBlock;
      }
      g_warning("read returned %d", errno);
      return errSSLInternal;
    }
    len += res;
  }
  *dataLength = len;
  return noErr;
}

OSStatus _SSLWriteFunction(SSLConnectionRef connection, void *data, size_t *dataLength)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)connection;
  
  size_t len = 0;
  while (len < *dataLength) {
    size_t chunk = (*dataLength - len < 4096 ? *dataLength - len : 4096);
    size_t res = write(channel->fd, ((char*)data)+len, chunk);
    
    if (res == -1) {
      if (errno == EAGAIN) {
        *dataLength = len;
        return errSSLWouldBlock;
      }
      g_warning("write returned %d", errno);
      return errSSLInternal;
    }
    len += res;
  }
  *dataLength = len;
  return noErr;
}

static void irssi_ssl_free(GIOChannel *handle)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)handle;
	g_io_channel_unref(channel->giochan);
  SSLDisposeContext(channel->context);
	g_free(channel);
}

static GIOStatus irssi_ssl_read(GIOChannel *handle, gchar *buf, gsize len, gsize *ret, GError **gerr)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)handle;
  size_t processed;
  OSStatus status;
  
  status = SSLRead(channel->context, buf, len, &processed);
  if (status == noErr) {
    *ret = processed;
    return G_IO_STATUS_NORMAL;
  }
  
  *ret = 0;
  
  if (status == errSSLWouldBlock) {
    return G_IO_STATUS_AGAIN;
  }
  
  if (status == errSSLClosedGraceful) {
    return G_IO_STATUS_EOF;
  }
  
  /* TODO: error message */
  return G_IO_STATUS_ERROR;
}

static GIOStatus irssi_ssl_write(GIOChannel *handle, const gchar *buf, gsize len, gsize *ret, GError **gerr)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)handle;
  size_t processed;
  OSStatus status;
  
  status = SSLWrite(channel->context, buf, len, &processed);
  if (status == noErr) {
    *ret = processed;
    return G_IO_STATUS_NORMAL;
  }
  
  *ret = 0;
  
  if (status == errSSLWouldBlock) {
    return G_IO_STATUS_AGAIN;
  }

  return G_IO_STATUS_ERROR;
}

static GIOStatus irssi_ssl_seek(GIOChannel *handle, gint64 offset, GSeekType type, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
  
	return chan->giochan->funcs->io_seek(handle, offset, type, gerr);
}

static GIOStatus irssi_ssl_close(GIOChannel *handle, GError **gerr)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
  
	return chan->giochan->funcs->io_close(handle, gerr);
}

static GSource *irssi_ssl_create_watch(GIOChannel *handle, GIOCondition cond)
{
	GIOSSLChannel *chan = (GIOSSLChannel *)handle;
  
	return chan->giochan->funcs->io_create_watch(handle, cond);
}

static GIOStatus irssi_ssl_set_flags(GIOChannel *handle, GIOFlags flags, GError **gerr)
{
  GIOSSLChannel *chan = (GIOSSLChannel *)handle;
  
  return chan->giochan->funcs->io_set_flags(handle, flags, gerr);
}

static GIOFlags irssi_ssl_get_flags(GIOChannel *handle)
{
  GIOSSLChannel *chan = (GIOSSLChannel *)handle;
  
  return chan->giochan->funcs->io_get_flags(handle);
}

static GIOFuncs irssi_ssl_channel_funcs = {
  irssi_ssl_read,
  irssi_ssl_write,
  irssi_ssl_seek,
  irssi_ssl_close,
  irssi_ssl_create_watch,
  irssi_ssl_free,
  irssi_ssl_set_flags,
  irssi_ssl_get_flags
};

int irssi_ssl_handshake(GIOChannel *handle)
{
  GIOSSLChannel *channel = (GIOSSLChannel*)handle;
  
  /* irssi can, and does, open us non-block and SSLHandshake won't like that */
  OSStatus status;
  int ret;
  
  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(channel->fd, &write_set);
  
  struct timeval timeout = { 0, 0 };
  
  while ((ret = select(channel->fd+1, NULL, &write_set, NULL, &timeout)) < 1)
  {
    if (ret == 0 || errno == EAGAIN) {
      g_warning("select() returned EAGAIN");
      return 3;
    }
    g_warning("select() waiting for socket connect returned %d.", errno);
    return -1;
  }
  
  /* again, we can be non-block and we've been asked to handshake, loop ... */
  status = SSLHandshake(channel->context);
  if (status != noErr) {
    if (status == errSSLWouldBlock) {
      /* openssl can tell if we want read or write, I can't */
      return 1;
    }
    g_warning("SSLHandshake failed with error %d.", status);
    return -1;
  }
  
  SecTrustRef secTrustRef;
  status = SSLCopyPeerTrust(channel->context, &secTrustRef);
  if (status != noErr) {
    g_warning("SSLCopyPeerTrust failed, unable to verify client cerfiticates. %d", status);
    return -1;
  }
  
  SecTrustResultType trustResult;
  status = SecTrustEvaluate(secTrustRef, &trustResult);
  if (status != noErr) {
    g_warning("SecTrustEvaluate failed, unable to verify client certificates. %d", status);
    CFRelease(secTrustRef);
    return -1;
  }
    
  switch (trustResult) {
    case kSecTrustResultProceed:
    case kSecTrustResultUnspecified:
      /* Happy with this certificate, carry on Sir ... */
      break;
    default:
    {
      /* Not so happy with this one */
      int code = irssibridge_present_trust_panel(secTrustRef);
      if (code != 1 /* NSOKButton */) {
        CFRelease(secTrustRef);
        return -1;
      }
    }
  }
  
  CFRelease(secTrustRef);
  
  return 0;
}

static GIOChannel *irssi_ssl_get_iochannel(GIOChannel *handle, const char *hostname, const char *mycert, const char *mypkey, const char *cafile, const char *capath, gboolean verify)
{
  GIOSSLChannel *channel;
  GIOChannel *gChannel;
  SSLContextRef contextRef;
  int fd;
  
  if (!(fd = g_io_channel_unix_get_fd(handle))) {
    return NULL;
  }
  
  OSStatus status = SSLNewContext(FALSE, &contextRef);
  if (status != noErr) {
    g_error("Failed to create new SSLContextRef, %d.", status);
    return NULL;
  }
  
  status = SSLSetIOFuncs(contextRef, (SSLReadFunc)_SSLReadFunction, (SSLWriteFunc)_SSLWriteFunction);
  if (status != noErr) {
    g_error("Failed to set IOFuncs, %d.", status);
    SSLDisposeContext(contextRef);
    return NULL;
  }
  
  /* network-openssl.c explicitly disables kSSLProtocol2 */
  SSLSetProtocolVersionEnabled(contextRef, kSSLProtocol2, FALSE);
  
  SSLSetEnableCertVerify(contextRef, verify);
  SSLSetAllowsAnyRoot(contextRef, TRUE);
  
  /* if we've got mycert, or more, then we need to try and load them into an identity */
  if (mycert) {
    SecIdentityRef identity = CreateIdentityFromCommonName(mycert, mypkey);
    
    /* you've asked for a cert, its not valid, that's an error */
    if (!identity) {
      g_warning("Failed to create an identity out of supplied certificates.");
      SSLDisposeContext(contextRef);
      return NULL;
    }
    
    /* set this identity as the client certificate of the connection */
    CFTypeRef values[] = {
      identity
    };
    
    CFArrayRef certificates = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
    status = SSLSetCertificate(contextRef, certificates);
    if (status == noErr) {
      status = SSLSetEncryptionCertificate(contextRef, certificates);
    }
    
    CFRelease(certificates);
    CFRelease(identity);
    
    if (status != noErr) {
      g_warning("Failed to set certificate for SSL connection. %d", status);
      return NULL;
    }
  }
  
  /* TODO: CA file and paths, currently ignored during verification */
  
  channel = g_new0(GIOSSLChannel, 1);
  channel->fd = fd;
  channel->giochan = handle;
  channel->context = contextRef;
  channel->verify = verify;
  channel->hostname = hostname;
  
  gChannel = (GIOChannel*)channel;
  gChannel->funcs = &irssi_ssl_channel_funcs;
  g_io_channel_init(gChannel);
  gChannel->is_readable = gChannel->is_writeable = TRUE;
  gChannel->use_buffer = FALSE;
  
  status = SSLSetConnection(contextRef, (SSLConnectionRef)channel);
  if (status != noErr) {
    g_error("Failed to set SSLConnection, %d.", status);
    SSLDisposeContext(contextRef);
    return NULL;
  }
  
  return gChannel;
}

GIOChannel *net_connect_ip_ssl(IPADDR *ip, int port, const char* hostname, IPADDR *my_ip, const char *cert, const char *pkey, const char *cafile, const char *capath, gboolean verify)
{
  GIOChannel *handle, *ssl_handle;
  
  handle = net_connect_ip(ip, port, my_ip);
  if (handle == NULL) {
    return NULL;
  }
  
  ssl_handle = irssi_ssl_get_iochannel(handle, hostname, cert, pkey, cafile, capath, verify);
  if (ssl_handle == NULL) {
    g_io_channel_unref(handle);
    return NULL;
  }
  return ssl_handle;
}