import socket
import sounddevice as sd

# Setup sounddevice for in-memory hardware playback
audio_stream = sd.RawOutputStream(
    samplerate=24000, 
    channels=1, 
    dtype='int16'
)
audio_stream.start()

SERVER_IP = "127.0.0.1"
SERVER_PORT = 5000

client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

try:
    client_socket.connect((SERVER_IP, SERVER_PORT))
    print(f"[*] Connected to Audio Server at {SERVER_IP}:{SERVER_PORT}")

    while True:
        prompt = input("\n[You]: ").strip()
        if prompt.lower() == "quit":
            break
        if not prompt:
            continue

        client_socket.sendall(prompt.encode("utf-8"))
        print("[*] Listening to Gemini...")

        # Receive and play the audio stream live
        while True:
            chunk = client_socket.recv(4096)
            
            if not chunk:
                break
                
            if b"[END_OF_MESSAGE]" in chunk:
                clean_chunk = chunk.replace(b"\n[END_OF_MESSAGE]\n", b"").replace(b"[END_OF_MESSAGE]", b"")
                if clean_chunk:
                    audio_stream.write(clean_chunk)
                break
                
            # Write bytes directly to the sound card memory buffer
            audio_stream.write(chunk)

except ConnectionRefusedError:
    print(f"[!] Could not connect to {SERVER_IP}:{SERVER_PORT}.")
except KeyboardInterrupt:
    print("\n[*] Exiting.")
finally:
    # 4. Cleanly erase the in-memory streams
    client_socket.close()
    audio_stream.stop()
    audio_stream.close()