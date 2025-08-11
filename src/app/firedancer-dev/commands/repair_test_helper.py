import os
import socket
import struct
import subprocess
import json

def base58_decode(s):
    """Simple base58 decoder implementation"""
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    base_count = len(alphabet)
    decoded = 0
    multi = 1
    s = s[::-1]  # reverse string
    for char in s:
        if char not in alphabet:
            raise ValueError(f"Invalid base58 character: {char}")
        decoded += multi * alphabet.index(char)
        multi *= base_count

    # Convert to bytes
    h = hex(decoded)[2:]
    if len(h) % 2:
        h = '0' + h
    result = bytes.fromhex(h)

    # Handle leading zeros
    pad = 0
    for c in s[::-1]:  # back to original order
        if c == alphabet[0]:
            pad += 1
        else:
            break
    return b'\x00' * pad + result

def fetch_cluster_nodes():
    """Fetch cluster nodes using curl and Solana RPC"""
    try:
        payload = json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "getClusterNodes",
            "params": []
        })
        result = subprocess.run(
            [
                "curl", "https://api.testnet.solana.com",
                "-X", "POST",
                "-H", "Content-Type: application/json",
                "-d", payload
            ],
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            data = json.loads(result.stdout)
            return data.get("result", [])
        else:
            print(f"Curl failed: {result.stderr}")
            return []
    except Exception as e:
        print(f"Error fetching cluster nodes: {e}")
        return []

def write_nodes_to_file(nodes, filename, max_peers=3000):
    peer_count = 0
    with open(filename, "wb") as f:
        max_peers = min(max_peers, len(nodes))
        for node in nodes[:max_peers]:
            pubkey_b58 = node.get("pubkey", "")
            # Convert base58 pubkey to bytes
            try:
                pubkey_bytes = base58_decode(pubkey_b58)
            except Exception:
                continue
            # Truncate or pad to 32 bytes
            if len(pubkey_bytes) > 32:
                pubkey_bytes = pubkey_bytes[:32]
            elif len(pubkey_bytes) < 32:
                pubkey_bytes = pubkey_bytes.ljust(32, b'\0')
            # Get IP and serveRepair port
            ip_bytes = b'\0\0\0\0'
            port_bytes = b'\0\0'
            # The Solana RPC returns a "serveRepair" field for each node
            # which is in the form "IP:PORT" or sometimes just "IP"
            serve_repair = node.get("serveRepair", "")
            if not isinstance(serve_repair, str):
                serve_repair = ""
            if ":" in serve_repair:
                ip_str, port_str = serve_repair.rsplit(":", 1)
            else:
                ip_str, port_str = serve_repair, ""
            try:
                ip_bytes = socket.inet_aton(ip_str)
            except Exception:
                ip_bytes = b'\0\0\0\0'
            try:
                port = int(port_str)
                port_bytes = struct.pack("<H", port)
            except Exception:
                port_bytes = b'\0\0'
            # Compose the line: 32 bytes pubkey, 4 bytes ip, 2 bytes port
            line = pubkey_bytes + ip_bytes + port_bytes
            # Only write exactly 38 bytes per line
            if len(line) == 38:
                f.write(line)
                peer_count += 1
    # Set file permissions to 777 (rwx for all)
    try:
        os.chmod(filename, 0o777)
    except Exception as e:
        print(f"Warning: Could not set permissions on {filename}: {e}")
    return peer_count

if __name__ == "__main__":
    print("Fetching Solana testnet cluster nodes...")
    nodes = fetch_cluster_nodes()
    print(f"Retrieved {len(nodes)} cluster nodes")

    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(script_dir, "repair_peers.bin")

    print(f"Writing top 50 repair peers to: {out_path}")
    peer_count = write_nodes_to_file(nodes, out_path, max_peers=3000)
    print(f"Successfully wrote {peer_count} repair peers to binary file")


