"""
udp_server.py
Exemplo para disciplina de Sistemas em Tempo Real
Prof. Felipe Viel
Curso de Engenharia de Computação da Univali
Exemplo de Servidor utilizando protocolo de transporte UDP.
A ESP32 deverá se conectar nele usando o IP do computador usando o servidor.
    - Use ipconfig no Windows e ifconfig no Linux para saber o IP do seu computador (com a placa de rede adequada)
o IPv4 0.0.0.0 indica que o servidor irá escutar qualquer IP que enviar pacote para para na porta indicada.
A porta, que nesse exemplo é 6010, pode ser alterada para qualquer número dentro do intervalo de 2^16.
"""

"""
Implementações adicionadas por Daniel Henrique da Silva e Milene Emmel Rovedder:
- Lógica de Resposta para medição de RTT na ESP32.
- Lógica de Envio de Comandos 'FS_TASK' e 'NAV_PLAN'.
"""
import socket
import json
import time

ADDR = ("0.0.0.0", 6010)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(ADDR)
print("Aguardando UDP em", ADDR)

COMMAND_INTERVAL_S = 3 # Envia um comando a cada 3 segundos
last_command_time = time.time()
target_address = None # Armazena o endereço do último cliente para responder
command_index = 0
COMMANDS = ["FS_TASK", "NAV_PLAN", "TELEMETRY"]

while True:
    try: # Recebe o log e responde para medir RTT
        data, src = s.recvfrom(2048)
        log_data = data.decode(errors="ignore")
        target_address = src 
        
        # Imprime o JSON recebido (simples)
        print(f"[{time.strftime('%H:%M:%S')}] LOG: {json.loads(log_data.strip())}")

        # Envia 'ECO' para a ESP32 medir o RTT
        s.sendto(b"ECO", src)

    except socket.timeout:
        pass
    except Exception as e:
        print(f"Erro ao receber/processar pacote: {e}")
        time.sleep(1)
        continue

    # Envia o comando periodicamente, alternado e a cada 3s
    current_time = time.time()
    if current_time - last_command_time >= COMMAND_INTERVAL_S and target_address:
        
        command_to_send = COMMANDS[command_index]
        try:
            print(f"[{time.strftime('%H:%M:%S')}] ENVIANDO COMANDO: {command_to_send}")
            s.sendto(command_to_send.encode(), target_address)
            
            # Atualiza o estado e o timer
            command_index = (command_index + 1) % len(COMMANDS)
            last_command_time = current_time
        except Exception as e:
            print(f"Erro ao enviar comando: {e}")
            time.sleep(1)