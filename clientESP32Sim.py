import socket
import json
import sounddevice as sd
import queue
import threading

# Network configuration
SERVER_IP = '127.0.0.1'
SERVER_PORT = 5000       
SAMPLE_RATE = 24000      
CHANNELS = 1             
DTYPE = 'int16'          
FRAME_SIZE = CHANNELS * 2 

def run_client():
    print(f"Connecting to Server at {SERVER_IP}:{SERVER_PORT}...")
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.connect((SERVER_IP, SERVER_PORT))
            print("Connected. Type your prompt below.\n")
        except ConnectionRefusedError:
            print("Connection refused. Ensure server_tcp is running first.")
            return

        while True:
            try:
                user_prompt = input("You: ")
                if not user_prompt.strip():
                    continue
                if user_prompt.lower() in ['exit', 'quit']:
                    break
                    
                sock.sendall(user_prompt.encode('utf-8'))
                
                network_buffer = bytearray()
                audio_buffer = bytearray()
                state = "WAITING_JSON"
                
                # 1. Thread Synchronization Queue
                audio_queue = queue.Queue()
                
                # 2. Dedicated Consumer Thread for Audio Playback
                def audio_worker():
                    # Setting latency='high' specifically combats VM timing drift
                    with sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, 
                                            dtype=DTYPE, latency='high') as stream:
                        while True:
                            chunk = audio_queue.get()
                            if chunk == b'STOP':
                                break
                            if chunk:
                                stream.write(chunk)
                                
                playback_thread = threading.Thread(target=audio_worker)
                playback_thread.start()
                
                # 3. Main Producer Thread (Network I/O)
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        print("\n[!] Server disconnected.")
                        audio_queue.put(b'STOP')
                        return
                        
                    network_buffer.extend(chunk)
                    
                    if state == "WAITING_JSON":
                        start_tag = b'[JSON_START]\n'
                        end_tag = b'\n[JSON_END]\n'
                        
                        if end_tag in network_buffer:
                            start_idx = network_buffer.find(start_tag)
                            end_idx = network_buffer.find(end_tag)
                            
                            if start_idx != -1 and end_idx != -1:
                                json_bytes = network_buffer[start_idx + len(start_tag) : end_idx]
                                try:
                                    data = json.loads(json_bytes.decode('utf-8'))
                                    print(f"\nBrain: {data.get('LLM_textual_answer', '')}")
                                except json.JSONDecodeError:
                                    pass
                                
                                network_buffer = network_buffer[end_idx + len(end_tag):]
                                state = "STREAMING_AUDIO"
                                print("[*] Playing audio...")

                    if state == "STREAMING_AUDIO":
                        eom_tag = b'\n[END_OF_MESSAGE]\n'
                        eom_idx = network_buffer.find(eom_tag)
                        is_eom = (eom_idx != -1)
                        
                        if is_eom:
                            audio_buffer.extend(network_buffer[:eom_idx])
                            network_buffer = network_buffer[eom_idx + len(eom_tag):]
                        else:
                            safe_to_process = max(0, len(network_buffer) - 20)
                            if safe_to_process > 0:
                                audio_buffer.extend(network_buffer[:safe_to_process])
                                del network_buffer[:safe_to_process]
                        
                        valid_length = len(audio_buffer) - (len(audio_buffer) % FRAME_SIZE)
                        
                        if valid_length > 0:
                            # Push data to the playback thread without blocking the network loop
                            audio_queue.put(bytes(audio_buffer[:valid_length]))
                            del audio_buffer[:valid_length]
                            
                        if is_eom:
                            print("\n--- Turn Complete ---\n")
                            # Signal the background thread to safely close the audio stream
                            audio_queue.put(b'STOP')
                            playback_thread.join() 
                            break 

            except KeyboardInterrupt:
                print("\nClient terminated by user.")
                break
            except Exception as e:
                print(f"\n[!] Client Error: {e}")
                break

if __name__ == "__main__":
    run_client()