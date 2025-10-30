import socket
import json
import time

PC_IP = "0.0.0.0"
PC_PORT = 6010
COMMAND_INTERVAL_S = 3 # Envia um comando a cada 3 segundos
COMMANDS = ["FS_TASK", "NAV_PLAN", "TELEMETRY"]

while True:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((PC_IP, PC_PORT))
        s.listen(1)
        print(f"Aguardando conexão em {PC_IP}:{PC_PORT}")
        
        # O servidor aceita a conexão TCP
        conn, addr = s.accept()
        print(f"Cliente {addr} conectado.")
        
        # Timeout para recv (permite que o loop verifique a hora para enviar comandos)
        conn.settimeout(0.5) 
        buffer = ""
        last_command_time = time.time()
        command_index = 0

        while True:
            try:
                # Recebe os dados da ESP32
                data = conn.recv(1024)
                
                if not data:
                    print("Cliente desconectou.")
                    break
                
                # Envia ECO de volta para a ESP32
                try:
                    conn.sendall(b"ECO\n") 
                except Exception as e:
                    print(f"Erro ao enviar ECO: {e}")
                    break
                # --------------------------------------------------
                
                buffer += data.decode(errors="ignore")
                
                # Processa os logs linha por linha (um JSON por linha)
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    if line:
                        try:
                            # Imprime o log formatado
                            log_data = json.loads(line)
                            print(f"[{time.strftime('%H:%M:%S')}] LOG: {log_data}")
                        except json.JSONDecodeError:
                            print(f"Erro de decodificação JSON: {line}")
                            
            except socket.timeout:
                # Quando não há dados, permite que o programa avance para o envio de comandos
                pass
            except (ConnectionResetError, OSError):
                print("Conexão perdida, aguardando novo cliente...")
                break

            # Envia Comandos Periódicos (NAV_PLAN, FS_TASK, TELEMETRY)
            if time.time() - last_command_time >= COMMAND_INTERVAL_S:
                cmd = COMMANDS[command_index]
                print(f"[{time.strftime('%H:%M:%S')}] ENVIANDO COMANDO: {cmd}")
                
                try:
                    # Envia o comando, incluindo o terminador de linha
                    conn.sendall((cmd + "\n").encode())
                    
                    # Atualiza o estado
                    command_index = (command_index + 1) % len(COMMANDS)
                    last_command_time = time.time()
                except Exception:
                    print("Falha ao enviar comando, conexão perdida.")
                    break