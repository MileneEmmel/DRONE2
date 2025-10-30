import socket
import json
import time

PC_IP = "0.0.0.0" 
PC_PORT = 6010 # Porta do #define PC_SERVER_PORT

# Cria o socket TCP
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# Permite que o servidor reutilize o endereço imediatamente (evita erro de "Address already in use")
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((PC_IP, PC_PORT))
# Fica ouvindo por conexões (1 é o número de conexões pendentes permitidas)
s.listen(1)
print("--- TCP Servidor Ativo ---")

# Lista de comandos para enviar periodicamente
COMMAND_INTERVAL_S = 3
COMMANDS = ["FS_TASK", "NAV_PLAN", "TELEMETRY"]

while True:
    print(f"\nAguardando conexão em {PC_IP}:{PC_PORT}")
    
    try:
        # Bloqueia a execução e espera por um cliente
        conn, addr = s.accept()
        print(f"Cliente {addr} conectado.")
    except Exception as e:
        print(f"Erro ao aceitar conexão: {e}")
        time.sleep(1)
        continue # Volta ao início do 'while True'

    # Bloco 'with conn' garante que a conexão será fechada ao sair
    with conn:
        # Define um timeout curto (0.5s) para que conn.recv() não bloqueie o loop
        conn.settimeout(0.5)
        buffer = "" # Buffer para acumular dados recebidos
        
        # Variáveis de estado para este cliente específico
        last_command_time = time.time()
        command_index = 0
        
        # Loop de comunicação: dura enquanto o cliente estiver conectado
        while True:
            try: 
                # Recebe dados do cliente
                data = conn.recv(1024) 
                if not data:
                    # Cliente desconectou de forma limpa (enviou pacote FIN)
                    print("Cliente encerrou a conexão.")
                    break # Sai do loop de comunicação
                
                # Acumula os dados recebidos no buffer
                buffer += data.decode('utf-8', errors='ignore')
                
                # Processa os logs (JSON)
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1) # Pega um JSON completo (terminado em \n)
                    if line:
                        log_data = line.strip()
                        try:
                            # Analisa o JSON
                            log_json = json.loads(log_data)
                            print(f"[{time.strftime('%H:%M:%S')}] LOG: {log_json}")

                            # Pega o 'seq' e envia o ECO para RTT
                            seq_num = log_json.get("seq")
                            if seq_num is not None:
                                eco_reply = f"ECO:{seq_num}\n"
                                conn.sendall(eco_reply.encode())
                                
                        except json.JSONDecodeError:
                            print(f"Erro: Recebido dado não-JSON: {log_data}")
                        except Exception as e:
                            print(f"Erro ao processar log ou enviar ECO: {e}")

            except socket.timeout:
                # Nenhum dado chegou nos últimos 0.5s, ignora e continua o loop para enviar comandos.
                pass
            except ConnectionResetError:
                # Cliente desconectou abruptamente (ex: ESP32 resetou)
                print("Erro: Conexão resetada pelo cliente.")
                break # Sai do loop de comunicação
            except Exception as e:
                # Outro erro inesperado de rede
                print(f"Erro de conexão/recebimento: {e}")
                break # Sai do loop de comunicação

            # Envio periódico de comandos
            current_time = time.time()
            if current_time - last_command_time >= COMMAND_INTERVAL_S:
                command_to_send = COMMANDS[command_index]
                
                try:
                    print(f"[{time.strftime('%H:%M:%S')}] ENVIANDO COMANDO: {command_to_send}")
                    # Envia o comando (com \n para a ESP32 poder ler)
                    conn.sendall((command_to_send + "\n").encode()) 
                    
                    # Atualiza o estado e o timer
                    command_index = (command_index + 1) % len(COMMANDS)
                    last_command_time = current_time
                except Exception as e:
                    # Falha ao enviar (provavelmente cliente desconectou)
                    print(f"Erro ao enviar comando: {e}")
                    break # Sai do loop de comunicação

    print(f"Cliente {addr} desconectado.")