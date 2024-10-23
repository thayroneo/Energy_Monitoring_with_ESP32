#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ZMPT101B.h>
#include <SPI.h>
#include <MFRC522.h>
#include <IOXhop_FirebaseESP32.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// Definindo pinos do OLED
#define I2C_SDA 17
#define I2C_SCL 16
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);  //Insancia objeo display

// Definindo pinos do RFID, relé e do Sensor de corrente
#define CURRENT_PIN 35 // Pino ADC conectado ao OUT do ACS712-20A
#define SS_PIN 21               // SDA do RFID
#define RST_PIN 22              // RST do RFID
#define RELAY_PIN 5             // Pino onde o relé está conectado
MFRC522 rfid(SS_PIN, RST_PIN);  // Instancia o objeto rfid

// Sensor de tensão ZMPT101B
#define SENSITIVITY 785.75f
ZMPT101B voltageSensor(34, 60.0);  // (Pino do Sensor, Frequencia da Rede)

#define FIREBASE_HOST ""  // Link do DB
#define FIREBASE_AUTH ""                    // Chave de Auenicação do DB

// Nome e senha do WiFi
const char* wifi_ssid = "";
const char* wifi_pass = "";

// UID dos cartões autorizados
String card1 = "B6073000";
String card2 = "936A222D";

bool relayState = false;  // Estado do relé (true para aberto e false para fechado)

// Definir valores fixos para corrente e tarifa
float energyCostPerKwh = 0.731980;  // Tarifa de energia em R$/kWh

// Variáveis
unsigned long relayStartTime = 0;  // Armazena o tempo de início do relé
unsigned long elapsedTime = 0;
float elapsedHours = 0.0;
float zeroOffset = 0;
float averagePower = 0.0;
float totalEnergyConsumed = 0.0;      // Energia acumulada em kWh
unsigned long totalPowerSum = 0.0;            // Soma das potências para o cálculo da média
unsigned long powerMeasurements = 0;  // Número de medições realizadas
float totalCost = 0.0;                // Inicializa a variável totalCost
String content = "";
int countSessionsID = 0;

// Variáveis para armazenar os dois horários
String entryDate_DMY, exitDate_DMY;
String entryTime_HMS, exitTime_HMS;
int entryDay, entryMonth, entryYear, exitDay, exitMonth, exitYear;
time_t entryTime_stamp, exitTime_stamp;  // Para realizar a diferença entre os horários

void setup() {
  Serial.begin(115200);

  // Inicialize o display
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;  // Não prossiga, loop infinito
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.display();

  // Inicializa o sensor de tensão
  voltageSensor.setSensitivity(SENSITIVITY);

  // Inicializa a comunicação SPI para o RFID
  SPI.begin();
  rfid.PCD_Init();

  //Inicialia a conexão com o WiFi
  WiFi.begin(wifi_ssid, wifi_pass);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (i > 20) {
      Serial.println("Conexão WiFi não esbelecida.");
      ESP.restart();
    }
    i++;
  }
  Serial.println("WiFi conectado");
  Serial.println("Endereço IP: ");
  Serial.println(WiFi.localIP());

  Firebase.begin(FIREBASE_HOST, FIREBASE_AUTH);  // Inicializa o BD

  // Configura o NTP
  configTime(-10800, 0, "pool.ntp.org");  // (Fuso horário do Brasil, Horário de Verão, Servidor NTP)

  // Configura o pino do relé como saída e garante que ele inicie desligado
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  relayState = false;

  zeroOffset = analogRead(CURRENT_PIN) * (3.3/4095); // Valor de tensão de referência
  Serial.println("-> Offset: " + String(zeroOffset));

}

void loop() {

  // Variável para armazenar o UID do cartão que ativou o sistema
  static String activeCardUID = "";  // Mantém o UID do cartão que ativou o relé

  // Verifica se há um novo cartão RFID
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {

    // Obtém o UID do cartão lido
    content = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      content += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");  // Adiciona um zero à esquerda se necessário
      content += String(rfid.uid.uidByte[i], HEX);               // Concatena o byte em formato hexadecimal
    }
    content.toUpperCase();  // Converte o UID lido para maiúsculas

    // Verifica se o cartão é autorizado
    if (content == card1 || content == card2) {

      // Se o relé estiver desligado (sistema inativo)
      if (!relayState) {

        // Armazena o UID do cartão que ativou o relé
        activeCardUID = content;
        countSessionsID++;

        Serial.println("Cartão autorizado! Ativando o relé...");
        display.clearDisplay();
        display.print(F("Cartao autorizado!"));
        display.display();
        delay(1000);

        // Ativa o relé -> Atualiza o estado do relé para Fechado
        digitalWrite(RELAY_PIN, HIGH);
        relayState = true;

        // Registra o dia e a hora de entrada
        getTimeInfo(entryDate_DMY, entryTime_HMS, entryDay, entryMonth, entryYear, entryTime_stamp);
        Serial.println("Entrada: " + String(entryDate_DMY) + " | " + String(entryTime_HMS));

        // Registra o tempo de início
        relayStartTime = millis();  // Só atualiza o tempo se o relé estava desligado
        Serial.println("Relé ativado.");

      } else if (content == activeCardUID) {  // Verifica se é o mesmo cartão que ativou o relé

        // Caso o relé já esteja ligado e o mesmo cartão que ativou tentar desativar
        Serial.println("Cartão autorizado!");
        Serial.println("Desativando o relé...");

        display.clearDisplay();
        display.setCursor(0, 0);
        display.print(F("Cartao autorizado!"));
        display.setCursor(0, 10);
        display.print(F("Encerrando sessao..."));
        display.display();
        delay(1000);

        getTimeInfo(exitDate_DMY, exitTime_HMS, exitDay, exitMonth, exitYear, exitTime_stamp);
        double spentTime = difftime(exitTime_stamp, entryTime_stamp);
        float spentTime_h = spentTime / 3600;

        Serial.println("Saída: " + String(exitTime_HMS));
        Serial.println("Tempo gasto: " + String(spentTime) + " Segundos (" + String(spentTime_h) + " Horas)");

        sendDataToFirebase(countSessionsID, content, totalEnergyConsumed, totalCost, entryDate_DMY, entryTime_HMS, exitTime_HMS, energyCostPerKwh, spentTime);

        // Desativa o relé -> Atualiza o estado do relé para Aberto
        digitalWrite(RELAY_PIN, LOW);
        relayState = false;

        activeCardUID = "";  // Limpa o UID do cartão ativo
        Serial.println("Relé desativado.");

        powerMeasurements = 0;
        totalPowerSum = 0;

      } else {
        // Caso o cartão não seja o mesmo que ativou o relé, o sistema rejeita a desativação
        Serial.println("Cartão não autorizado para desativar o sistema!");
        display.clearDisplay();
        display.print(F("Nao autorizado para desligar."));
        display.display();
        delay(1000);
      }

    } else {
      // Cartão não autorizado
      Serial.println("Cartão não autorizado!");
      display.clearDisplay();
      display.setCursor(0, 0);
      display.print(F("Cartao nao autorizado!"));
      display.display();
      delay(1000);
    }

    // Para o processamento do cartão
    rfid.PICC_HaltA();
  }

  // Se o relé estiver ativado, monitora a tensão
  if (relayState) {
    float voltage = voltageSensor.getRmsVoltage();
    if (voltage < 198) {
      voltage = 0;
    }
    calculateAndPrintCost(voltage);

  } else {
    Serial.println("Aproxime o seu Cartão.");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print(F("Aproxime o seu Cartao"));
    display.display();

    float voltage = voltageSensor.getRmsVoltage();
    if (voltage < 198) {
      voltage = 0;
    }
    Serial.println("Tensão: " + String(voltage) + " V");
  }

  delay(1000);
}

// Função para calcular o consumo de energia e o custo ao longo do tempo
void calculateAndPrintCost(float voltage) {

  // Limpa o display antes de atualizar os dados
  display.clearDisplay();

  float current = calculateRMSCurrent();

  if (voltage == 0) {
    averagePower = 0.0;
  } else {
    float power = voltage * current;  // Potência em watts
    totalPowerSum += power;           // Acumula a potência total
    powerMeasurements++;              // Incrementa o número de medições

    // Calcula a potência média
    averagePower = totalPowerSum / powerMeasurements;
  }

  // Calcula o tempo decorrido em segundos e horas
  unsigned long elapsedTime = (millis() - relayStartTime) / 1000;  // Milisegundos para Segunos
  float elapsedHours = elapsedTime / 3600.0;                       // Converte tempo em segundos para horas

  // Calcula a energia consumida em kWh
  float energyConsumed = (averagePower / 1000.0) * elapsedHours;  // Energia consumida em kWh
  totalEnergyConsumed = energyConsumed;                           // Acumula a energia total consumida
  totalCost = totalEnergyConsumed * energyCostPerKwh;             // Calcula o custo total

  // Exibe os valores no Monitor Serial
  Serial.println("------------------------------");
  Serial.println("UID: " + String(content) + " | Entrada: " + String(entryDate_DMY) + " - " + String(entryTime_HMS));
  Serial.println("Tensão: " + String(voltage) + " V");
  Serial.println("Corrente: " + String(current) + " A");
  Serial.println("Potência Total: " + String(totalPowerSum) + " W");
  Serial.println("Medições: " + String(powerMeasurements));
  Serial.println("Média da Potência: " + String(averagePower) + " W");
  Serial.println("Consumo de Energia: " + String(totalEnergyConsumed) + " kWh");
  Serial.println("Custo estimado: R$ " + String(totalCost));
  Serial.println("Tempo decorrido: " + String(elapsedTime) + " segundos (" + String(elapsedHours) + " horas)");
  Serial.println("------------------------------");
  Serial.println(" ");

  // Exibir no display
  display.setCursor(0, 0);
  display.print(F("Tensao: "));
  display.print(voltage);
  display.print(F(" V"));

  display.setCursor(0, 10);
  display.print(F("Corrente: "));
  display.print(current);
  display.print(F(" A"));

  display.setCursor(0, 20);
  display.print(F("Energia: "));
  display.print(totalEnergyConsumed);
  display.print(F(" KWh"));

  display.setCursor(0, 30);
  display.print(F("Pot Med: "));
  display.print(averagePower);
  display.print(F(" W"));

  display.setCursor(0, 40);
  display.print(F("Custo: R$ "));
  display.print(totalCost);

  display.setCursor(0, 50);
  display.print(F("Tempo: "));
  display.print(elapsedTime);
  display.print(F(" s ("));
  display.print(elapsedHours);
  display.print(F(" h)"));

  // Atualiza o display
  display.display();
}

// Função para enviar os dados para o BD
void sendDataToFirebase(int countSessionsID, String content, float totalEnergyConsumed, float totalCost, String entryDateString, String entryTimeString, String exitTimeString, float energyCostPerKwh, double spentTime) {

  String defaultPath = "/sessions/sessao_" + String(countSessionsID) + "/";
  String morador = Firebase.getString("/moradores/" + String(content) + "/nome");

  float spentTime_h = spentTime/3600;

  Firebase.setString(defaultPath + "morador", morador);
  Firebase.setString(defaultPath + "UID_morador", content);
  Firebase.setFloat(defaultPath + "consumo_Kwh", totalEnergyConsumed);
  Firebase.setFloat(defaultPath + "custo", totalCost);
  Firebase.setString(defaultPath + "data_uso", entryDateString);
  Firebase.setString(defaultPath + "horarioEntrada", entryTimeString);
  Firebase.setString(defaultPath + "horarioSaida", exitTimeString);
  Firebase.setFloat(defaultPath + "tarifa", energyCostPerKwh);
  Firebase.setFloat(defaultPath + "tempoGasto_h", spentTime_h);
}

//Funcão para o obter horário e data
void getTimeInfo(String& dateFormat_DMY, String& time_HMS, int& day, int& month, int& year, time_t& nowTime) {

  time_t rawtime;
  time(&rawtime);
  struct tm timeInfo;
  localtime_r(&rawtime, &timeInfo);

  dateFormat_DMY = String(timeInfo.tm_mday) + "/" + String(timeInfo.tm_mon + 1) + "/" + String(timeInfo.tm_year + 1900);
  time_HMS = String(timeInfo.tm_hour) + ":" + String(timeInfo.tm_min) + ":" + String(timeInfo.tm_sec);

  day = timeInfo.tm_mday;
  month = (timeInfo.tm_mon + 1);
  year = (timeInfo.tm_year + 1900);

  nowTime = rawtime;
}

float calculateRMSCurrent(){

  float current = 0.0;
  float sumCurrentSquares = 0.0; 
  float sensitivity = 100.0; // Sensibiblidade do ACS712-20A (100 [mV/A])
  float analogValue = 0.0;
  float analogVoltValue = 0.0;
  const int numCurrentMeasurements = 1000;

  for(int i=0; i<numCurrentMeasurements; i++){

    analogValue = analogRead(CURRENT_PIN);
    analogVoltValue = analogValue*(3.3/4095);
    analogVoltValue = analogVoltValue - zeroOffset;
    current = analogVoltValue / (sensitivity/1000);
    sumCurrentSquares += current*current;

    delayMicroseconds(167); // Periodo da onda (1/60Hz) em milissegundos
  }

  float RMSCurrent = sqrt(sumCurrentSquares / numCurrentMeasurements);

  if(RMSCurrent < 1){
    return 0.0;
  } else {
    return RMSCurrent;
  }
  
}
