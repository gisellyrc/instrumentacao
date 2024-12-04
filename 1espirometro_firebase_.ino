#include <WiFi.h>
#include <FirebaseESP32.h>
#include <time.h>

// Dados da rede Wi-Fi
const char* ssid = "Nome da rede";
const char* password = "Senha da rede";

// Dados do Firebase
#define FIREBASE_HOST "link do banco de dados"
#define FIREBASE_AUTH "chave de acesso"

// Configurações do Firebase
FirebaseConfig config;
FirebaseAuth auth;
FirebaseData firebaseData;

// Variáveis globais
#define SENSOR 27
long currentMillis = 0;
long previousMillis = 0;
int interval = 1000;  // Intervalo de 1 segundo
volatile byte pulseCount = 0;  // Contador de pulsos
float calibrationFactor = 650;
float flowRate = 0.0, volume = 0.0, maxFlowRate = 0.0, totalFlowRate = 0.0;
int totalSeconds = 0;
bool readingEnabled = false;
String patientName = "";  // Nome do paciente

// Variáveis para detecção de fluxo finalizado
unsigned long noFlowStartTime = 0;
const unsigned long noFlowThreshold = 1000;  // Tempo sem fluxo (1 segundo)

// Variáveis para configuração carimbo data/hora
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800; // Offset para o horário de Brasília (-3 horas)
const int daylightOffset_sec = 0;  // Sem horário de verão

// Protótipo da função
void saveDataToFirebase(float cvf, float fefMax, float fefMed, float tef, float vef1);

// Função de interrupção
void IRAM_ATTR pulseCounter() {
    pulseCount++;
}

void setup() {
    Serial.begin(115200);
    pinMode(SENSOR, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SENSOR), pulseCounter, FALLING);

    // Conexão Wi-Fi
    Serial.print("Conectando-se a WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("\nWiFi conectado.");
    Serial.print("Endereço IP: ");
    Serial.println(WiFi.localIP());

    // Configuração do Firebase
    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;

    Firebase.begin(&config, &auth);
    Serial.println("Sistema inicializado. Aguardando leituras...");

    // Configuração data/hora
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Sincronizando horário com NTP...");
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Falha ao sincronizar horário.");
        return;
    }
    Serial.println("Horário sincronizado.");
}

void loop() {
    if (!readingEnabled) {
        if (pulseCount > 0) {
            // Solicitar o nome do paciente antes de iniciar nova leitura
            Serial.println("Insira o nome do paciente:");
            patientName = ""; // Limpar o nome anterior
            while (patientName == "") {
                if (Serial.available() > 0) {
                    patientName = Serial.readStringUntil('\n');
                    patientName.trim(); // Remove espaços e quebras de linha
                }
            }
            Serial.print("Nome do paciente configurado: ");
            Serial.println(patientName);

            // Configurar variáveis para nova leitura
            readingEnabled = true;
            pulseCount = 0;
            flowRate = 0.0;
            volume = 0.0;
            maxFlowRate = 0.0;
            totalFlowRate = 0.0;
            totalSeconds = 0;
            noFlowStartTime = 0;
            currentMillis = millis();
        }
        return;
    }

    currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;

        float pulses = pulseCount;
        pulseCount = 0;
        flowRate = pulses / calibrationFactor;  // Taxa de fluxo (L/s)
        totalFlowRate += flowRate;
        volume += flowRate * (interval / 1000.0);
        maxFlowRate = max(maxFlowRate, flowRate);
        totalSeconds++;

        // Rastrear o volume do primeiro segundo
        static float firstSecondVolume = 0.0;
        if (totalSeconds <= 3) {
            firstSecondVolume += flowRate * (interval / 1000.0); // Acumular somente no primeiro segundo
        }

        // Exibir dados no monitor serial
        Serial.print("Fluxo: ");
        Serial.print(flowRate, 2);
        Serial.print(" L/s, Volume acumulado: ");
        Serial.print(volume, 2);
        Serial.println(" L");

        // Monitorar fluxo finalizado
        if (flowRate == 0) {
            if (noFlowStartTime == 0) {
                noFlowStartTime = currentMillis; // Iniciar o contador de fluxo zero
            } else if (currentMillis - noFlowStartTime > noFlowThreshold) {
                // Finalizar leitura após tempo limite sem fluxo
                readingEnabled = false;

                float averageFlowRate = totalFlowRate / totalSeconds;
                float TEF = totalSeconds - 3;  // Tempo de expiração forçada
                float VEF_1 = firstSecondVolume; // Volume no primeiro segundo

                // Exibir os resultados no console
                Serial.println("\nLeitura concluída.");
                Serial.print("CVF: ");
                Serial.print(volume, 2);
                Serial.println(" L");
                Serial.print("FEF_max: ");
                Serial.print(maxFlowRate, 2);
                Serial.println(" L/s");
                Serial.print("FEF_med: ");
                Serial.print(averageFlowRate, 2);
                Serial.println(" L/s");
                Serial.print("TEF: ");
                Serial.print(TEF);
                Serial.println(" s");
                Serial.print("VEF_1: ");
                Serial.print(VEF_1, 2);
                Serial.println(" L");

                // Salvar dados no Firebase
                saveDataToFirebase(volume, maxFlowRate, averageFlowRate, TEF, VEF_1);

                Serial.println("Aguardando próximo fluxo...");
                Serial.println("\n");
                Serial.println("Insira o nome do paciente e pressione Enter");
            }
        } else {
            noFlowStartTime = 0; // Resetar o tempo de ausência de fluxo
        }
    }
}

// Função para salvar os dados no Firebase
void saveDataToFirebase(float cvf, float fefMax, float fefMed, float tef, float vef1) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Falha ao obter horário local.");
        return;
    }

    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

    String path = "/Dados_esp/" + patientName;
    String newPath = path + "/" + String(timestamp); // Usar o timestamp formatado como chave

    Firebase.setFloat(firebaseData, newPath + "/CVF", cvf);
    Firebase.setFloat(firebaseData, newPath + "/FEF_max", fefMax);
    Firebase.setFloat(firebaseData, newPath + "/FEF_med", fefMed);
    Firebase.setInt(firebaseData, newPath + "/TEF", tef);
    Firebase.setFloat(firebaseData, newPath + "/VEF_1", vef1);

    if (firebaseData.httpCode() != 200) {
        Serial.print("Erro ao enviar dados para o Firebase: ");
        Serial.println(firebaseData.errorReason());
    } else {
        Serial.println("Dados enviados para o Firebase com sucesso.");
    }
}
