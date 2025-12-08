#!/usr/bin/env python3
"""
Simple Python client to test KTLS server
Uses standard Python SSL library which should support KTLS on Linux
"""

import socket
import ssl
import sys


def test_ktls_echo_server(host='localhost', port=8080, ca_cert='/home/ynachi/test_certs/ca.crt'):
    """
    Test the KTLS echo server by sending a simple HTTP request
    """
    print(f"Connecting to {host}:{port}...")

    # Create a plain TCP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        # Connect to server
        sock.connect((host, port))
        print(f"✓ Connected to {host}:{port}")

        # Create SSL context
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.maximum_version = ssl.TLSVersion.TLSv1_3

        # Load CA certificate for verification
        context.load_verify_locations(ca_cert)
        context.check_hostname = True

        # Wrap socket with SSL
        print("Starting TLS handshake...")
        ssl_sock = context.wrap_socket(sock, server_hostname=host)

        print(f"✓ TLS handshake complete!")
        print(f"  Version: {ssl_sock.version()}")
        print(f"  Cipher: {ssl_sock.cipher()}")

        # Check if KTLS is enabled (Python 3.12+)
        try:
            # Note: This attribute may not be available in all Python versions
            if hasattr(ssl_sock, 'get_channel_binding'):
                print(f"  Channel binding available")
        except:
            pass

        # Send HTTP request
        request = (
            "GET / HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Connection: close\r\n"
            "\r\n"
        )

        print(f"\nSending request ({len(request)} bytes):")
        print(request)

        ssl_sock.sendall(request.encode())
        print("✓ Request sent")

        # Receive response
        print("\nWaiting for response...")
        response = ssl_sock.recv(8192)

        if response:
            print(f"✓ Received {len(response)} bytes:")
            print("-" * 60)
            print(response.decode('utf-8', errors='replace'))
            print("-" * 60)
            return True
        else:
            print("✗ No response received")
            return False

    except ssl.SSLError as e:
        print(f"✗ SSL Error: {e}")
        return False
    except Exception as e:
        print(f"✗ Error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        try:
            ssl_sock.close()
        except:
            sock.close()


def test_simple_echo(host='localhost', port=8080, ca_cert='/home/ynachi/test_certs/ca.crt'):
    """
    Even simpler test - just send some data and read it back
    """
    print(f"\n{'=' * 60}")
    print("SIMPLE ECHO TEST")
    print(f"{'=' * 60}\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        sock.connect((host, port))
        print(f"✓ Connected")

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.load_verify_locations(ca_cert)

        ssl_sock = context.wrap_socket(sock, server_hostname=host)
        print(f"✓ TLS 1.3 handshake complete - Cipher: {ssl_sock.cipher()[0]}")

        # Send simple message
        message = "Hello from Python!\n"
        print(f"\nSending: {message.strip()}")
        ssl_sock.sendall(message.encode())

        # Read echo back
        response = ssl_sock.recv(1024)
        print(f"Received: {response.decode().strip()}")

        if response.decode() == message:
            print("✓ Echo test PASSED!")
            return True
        else:
            print("✗ Echo test FAILED - response doesn't match")
            return False

    except Exception as e:
        print(f"✗ Error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        try:
            ssl_sock.close()
        except:
            sock.close()


def test_without_verification(host='localhost', port=8080):
    """
    Test without certificate verification (like your original C++ client)
    """
    print(f"\n{'=' * 60}")
    print("TEST WITHOUT CERTIFICATE VERIFICATION")
    print(f"{'=' * 60}\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    try:
        sock.connect((host, port))
        print(f"✓ Connected")

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE  # Disable verification

        ssl_sock = context.wrap_socket(sock, server_hostname=host)
        print(f"✓ TLS 1.3 handshake complete (no verification)")
        print(f"  Cipher: {ssl_sock.cipher()[0]}")

        # Send HTTP request like your C++ client
        request = "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        print(f"\nSending HTTP request ({len(request)} bytes)")
        ssl_sock.sendall(request.encode())
        print("✓ Request sent")

        # Try to read response
        print("Attempting to read response...")
        response = ssl_sock.recv(8192)

        if response:
            print(f"✓ Received {len(response)} bytes:")
            print(response.decode('utf-8', errors='replace'))
            return True
        else:
            print("✗ No response received (EOF)")
            return False

    except Exception as e:
        print(f"✗ Error: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        try:
            ssl_sock.close()
        except:
            sock.close()


if __name__ == "__main__":
    print("KTLS Server Test Client")
    print("=" * 60)

    # Test 1: Without verification (like your original C++ client)
    test_without_verification()

    # Test 2: Simple echo test
    test_simple_echo()

    # Test 3: HTTP request with verification
    test_ktls_echo_server()

    print("\n" + "=" * 60)
    print("All tests complete")
