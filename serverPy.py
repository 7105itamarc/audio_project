import socket
from google import genai
from google.genai import types

API_KEY = "PLACE HOLDER FOR AI API KEY"
client = genai.Client(api_key=API_KEY)

# Fast conversational text based model with memory
chat = client.chats.create(model="gemini-3.5-flash-lite")

# The Voice: Dedicated TTS model 
tts_chat = client.chats.create(model="gemini-3.1-flash-tts-preview")


HOST = "0.0.0.0"
PORT = 5000

server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server_socket.bind((HOST, PORT))
server_socket.listen(1)

print(f"[*] Audio Server listening on {HOST}:{PORT}...")

try:
    while True:
        conn, addr = server_socket.accept()
        print(f"\n[*] Client connected from: {addr}")

        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break

                prompt = data.decode("utf-8").strip()
                if not prompt:
                    continue

                print(f"\n[Client]: {prompt}")
                
                print("[*] Thinking...")
                text_response = chat.send_message(prompt).text
                print(f"[Gemini]: {text_response}")

                print("[*] Generating Audio...")
                
                # Convert the text prompt response to speech
                audio_stream = tts_chat.send_message_stream(
                    message=text_response,
                    config=types.GenerateContentConfig(
                        response_modalities=["AUDIO"],
                        speech_config=types.SpeechConfig(
                            voice_config=types.VoiceConfig(
                                prebuilt_voice_config=types.PrebuiltVoiceConfig(
                                    voice_name="Kore"
                                )
                            )
                        )
                    )
                )

                for chunk in audio_stream:
                    if chunk.candidates and chunk.candidates[0].content.parts:
                        for part in chunk.candidates[0].content.parts:
                            if part.inline_data:
                                conn.sendall(part.inline_data.data)

                conn.sendall(b"\n[END_OF_MESSAGE]\n")

        except Exception as e:
            print(f"[!] API Error: {e}")
        finally:
            conn.close()

except KeyboardInterrupt:
    print("\n[*] Shutting down server.")
finally:
    server_socket.close()