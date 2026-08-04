import React, { useState, useMemo } from 'react';
import {
  StyleSheet,
  View,
  Text,
  TextInput,
  TouchableOpacity,
  FlatList,
  KeyboardAvoidingView,
  Platform,
  SafeAreaView,
  StatusBar,
  Alert,
} from 'react-native';

// --- CONTRATO TYPESCRIPT OBRIGATÓRIO ---
export type IOTagType = 'DIGITAL_IN' | 'DIGITAL_OUT' | 'ANALOG_IN' | 'ANALOG_OUT';

export interface IOMapping {
  address: string;      // Padrão IEC (ex: "I0.0") -> Somente leitura na UI
  tag: string;          // Nome definido pelo usuário -> Editável via TextInput limpo
  type: IOTagType;      // Somente leitura
  physical_pin: string; // Hardware base (ex: "GPIO_34") -> Oculto ou texto minúsculo/suave para debug
}

// --- MOCK DATA INICIAL REALISTA (4 Entradas + 4 Saídas) ---
const INITIAL_MOCK_DATA: IOMapping[] = [
  { address: 'I0.0', tag: 'CHAVE_EMERGENCIA', type: 'DIGITAL_IN', physical_pin: 'GPIO_34' },
  { address: 'I0.1', tag: 'SENSOR_NIVEL_ALTO', type: 'DIGITAL_IN', physical_pin: 'GPIO_35' },
  { address: 'I0.2', tag: 'SENSOR_PRESENCA_P1', type: 'DIGITAL_IN', physical_pin: 'GPIO_32' },
  { address: 'I0.3', tag: 'PRESSOSTATO_LINHA', type: 'DIGITAL_IN', physical_pin: 'GPIO_33' },
  { address: 'Q0.0', tag: 'BOMBA_RECIRCULACAO', type: 'DIGITAL_OUT', physical_pin: 'GPIO_25' },
  { address: 'Q0.1', tag: 'VALVULA_SOLENOIDE_1', type: 'DIGITAL_OUT', physical_pin: 'GPIO_26' },
  { address: 'Q0.2', tag: 'TORRE_SINALIZACAO_VERDE', type: 'DIGITAL_OUT', physical_pin: 'GPIO_27' },
  { address: 'Q0.3', tag: 'ALARME_SONORO_PAINEL', type: 'DIGITAL_OUT', physical_pin: 'GPIO_14' },
];

export interface IOMappingScreenProps {
  onSave?: (mappings: IOMapping[]) => void;
}

export const IOMappingScreen: React.FC<IOMappingScreenProps> = ({ onSave }) => {
  const [mappings, setMappings] = useState<IOMapping[]>(INITIAL_MOCK_DATA);
  const [activeTab, setActiveTab] = useState<'IN' | 'OUT'>('IN');
  const [isSaving, setIsSaving] = useState(false);

  // Filtragem eficiente por aba
  const filteredData = useMemo(() => {
    return mappings.filter(item =>
      activeTab === 'IN'
        ? item.type === 'DIGITAL_IN' || item.type === 'ANALOG_IN'
        : item.type === 'DIGITAL_OUT' || item.type === 'ANALOG_OUT'
    );
  }, [mappings, activeTab]);

  // Contadores para os badges das abas
  const inCount = useMemo(
    () => mappings.filter(m => m.type.endsWith('_IN')).length,
    [mappings]
  );
  const outCount = useMemo(
    () => mappings.filter(m => m.type.endsWith('_OUT')).length,
    [mappings]
  );

  // Atualização local imediata do nome (tag) da I/O
  const handleTagChange = (address: string, newTag: string) => {
    setMappings(prev =>
      prev.map(item => (item.address === address ? { ...item, tag: newTag } : item))
    );
  };

  // Envio / Persistência no Gateway
  const handleSaveToDevice = () => {
    setIsSaving(true);
    const jsonPayload = JSON.stringify({ io_mappings: mappings }, null, 2);

    if (onSave) {
      onSave(mappings);
    }

    // Feedback visual limpo de salvamento
    setTimeout(() => {
      setIsSaving(false);
      Alert.alert(
        'Tabela Atualizada',
        `Mapeamento de I/O gravado com sucesso!\n\nPayload LittleFS:\n${jsonPayload.substring(0, 180)}...`,
        [{ text: 'OK' }]
      );
    }, 400);
  };

  // Renderizador de cada item da lista (Card Compacto Horizontal)
  const renderIOCard = ({ item }: { item: IOMapping }) => {
    const isInput = item.type.endsWith('_IN');

    return (
      <View style={styles.card}>
        {/* Lado Esquerdo: Endereço IEC e Badge de Hardware */}
        <View style={styles.cardHeaderInfo}>
          <View style={[styles.addressBadge, isInput ? styles.inBadge : styles.outBadge]}>
            <Text style={styles.addressText}>{item.address}</Text>
          </View>
          <Text style={styles.physicalPinText}>{item.physical_pin}</Text>
        </View>

        {/* Lado Direito: Input para Tag do Usuário */}
        <View style={styles.inputContainer}>
          <TextInput
            style={styles.tagInput}
            value={item.tag}
            onChangeText={(text) => handleTagChange(item.address, text)}
            placeholder="Nome da Tag (ex: SENSOR_P1)"
            placeholderTextColor="#64748B"
            autoCapitalize="characters"
            autoCorrect={false}
            spellCheck={false}
          />
        </View>
      </View>
    );
  };

  return (
    <SafeAreaView style={styles.safeArea}>
      <StatusBar barStyle="light-content" backgroundColor="#0F172A" />

      <KeyboardAvoidingView
        style={styles.keyboardContainer}
        behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
        keyboardVerticalOffset={Platform.OS === 'ios' ? 10 : 0}
      >
        {/* CABEÇALHO TÉCNICO */}
        <View style={styles.header}>
          <View style={styles.titleRow}>
            <Text style={styles.headerTitle}>ENDAP Studio</Text>
            <View style={styles.statusChip}>
              <View style={styles.statusDot} />
              <Text style={styles.statusText}>GATEWAY ONLINE</Text>
            </View>
          </View>
          <Text style={styles.headerSubtitle}>Mapeamento de I/O (IEC 61131-3)</Text>
        </View>

        {/* SELETOR DE ABAS (ENTRADAS / SAÍDAS) */}
        <View style={styles.tabBar}>
          <TouchableOpacity
            style={[styles.tabButton, activeTab === 'IN' && styles.activeTabButton]}
            onPress={() => setActiveTab('IN')}
            activeOpacity={0.8}
          >
            <Text style={[styles.tabText, activeTab === 'IN' && styles.activeTabText]}>
              Entradas
            </Text>
            <View style={[styles.countBadge, activeTab === 'IN' && styles.activeCountBadge]}>
              <Text style={[styles.countText, activeTab === 'IN' && styles.activeCountText]}>
                {inCount}
              </Text>
            </View>
          </TouchableOpacity>

          <TouchableOpacity
            style={[styles.tabButton, activeTab === 'OUT' && styles.activeTabButton]}
            onPress={() => setActiveTab('OUT')}
            activeOpacity={0.8}
          >
            <Text style={[styles.tabText, activeTab === 'OUT' && styles.activeTabText]}>
              Saídas
            </Text>
            <View style={[styles.countBadge, activeTab === 'OUT' && styles.activeCountBadge]}>
              <Text style={[styles.countText, activeTab === 'OUT' && styles.activeCountText]}>
                {outCount}
              </Text>
            </View>
          </TouchableOpacity>
        </View>

        {/* LISTA COMPACTA COM FLATLIST */}
        <FlatList
          data={filteredData}
          keyExtractor={(item) => item.address}
          renderItem={renderIOCard}
          contentContainerStyle={styles.listContent}
          showsVerticalScrollIndicator={false}
          keyboardShouldPersistTaps="handled"
        />

        {/* BOTÃO PRINCIPAL FIXO NA BASE */}
        <View style={styles.footerContainer}>
          <TouchableOpacity
            style={[styles.saveButton, isSaving && styles.saveButtonDisabled]}
            onPress={handleSaveToDevice}
            activeOpacity={0.85}
            disabled={isSaving}
          >
            <Text style={styles.saveButtonText}>
              {isSaving ? 'Gravando no LittleFS...' : 'Salvar Tabela no Dispositivo'}
            </Text>
          </TouchableOpacity>
        </View>
      </KeyboardAvoidingView>
    </SafeAreaView>
  );
};

// --- ESTILOS VISUAIS: DARK PROFESSIONAL (GRAPHITE / CYAN / TEAL) ---
const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
    backgroundColor: '#0F172A', // Grafite / Escuro Profissional (Slate 900)
  },
  keyboardContainer: {
    flex: 1,
  },
  header: {
    paddingHorizontal: 20,
    paddingTop: 16,
    paddingBottom: 12,
    borderBottomWidth: 1,
    borderBottomColor: '#1E293B',
  },
  titleRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  headerTitle: {
    color: '#00F5D4', // Cyan Neon / Primary Accent
    fontSize: 14,
    fontWeight: '800',
    letterSpacing: 1.2,
    textTransform: 'uppercase',
  },
  statusChip: {
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: '#064E3B',
    paddingHorizontal: 8,
    paddingVertical: 4,
    borderRadius: 12,
    borderWidth: 1,
    borderColor: '#10B981',
  },
  statusDot: {
    width: 6,
    height: 6,
    borderRadius: 3,
    backgroundColor: '#10B981',
    marginRight: 6,
  },
  statusText: {
    color: '#A7F3D0',
    fontSize: 10,
    fontWeight: '700',
    letterSpacing: 0.5,
  },
  headerSubtitle: {
    color: '#F8FAFC',
    fontSize: 18,
    fontWeight: '700',
    marginTop: 4,
  },
  tabBar: {
    flexDirection: 'row',
    marginHorizontal: 20,
    marginTop: 14,
    marginBottom: 10,
    backgroundColor: '#1E293B',
    borderRadius: 8,
    padding: 3,
    borderWidth: 1,
    borderColor: '#334155',
  },
  tabButton: {
    flex: 1,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'center',
    paddingVertical: 8,
    borderRadius: 6,
  },
  activeTabButton: {
    backgroundColor: '#00F5D4', // Cyan Accent
  },
  tabText: {
    color: '#94A3B8',
    fontSize: 14,
    fontWeight: '600',
  },
  activeTabText: {
    color: '#0F172A', // Texto escuro em cima do Cyan
    fontWeight: '700',
  },
  countBadge: {
    marginLeft: 8,
    backgroundColor: '#334155',
    paddingHorizontal: 6,
    paddingVertical: 2,
    borderRadius: 10,
  },
  activeCountBadge: {
    backgroundColor: '#0F172A',
  },
  countText: {
    color: '#CBD5E1',
    fontSize: 11,
    fontWeight: '700',
  },
  activeCountText: {
    color: '#00F5D4',
  },
  listContent: {
    paddingHorizontal: 20,
    paddingTop: 8,
    paddingBottom: 24,
  },
  card: {
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: '#1E293B', // Card Grafite Escuro
    borderRadius: 8,
    paddingHorizontal: 12,
    paddingVertical: 10,
    marginBottom: 8,
    borderWidth: 1,
    borderColor: '#334155',
  },
  cardHeaderInfo: {
    width: 86,
    justifyContent: 'center',
  },
  addressBadge: {
    paddingHorizontal: 6,
    paddingVertical: 3,
    borderRadius: 4,
    alignSelf: 'flex-start',
  },
  inBadge: {
    backgroundColor: '#0369A1', // Slate Blue Accent para Inputs
  },
  outBadge: {
    backgroundColor: '#0D9488', // Teal Accent para Outputs
  },
  addressText: {
    color: '#FFFFFF',
    fontSize: 13,
    fontWeight: '700',
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace', // Fonte monoespaçada técnica
  },
  physicalPinText: {
    color: '#64748B', // Texto minúsculo/suave para debug
    fontSize: 10,
    fontFamily: Platform.OS === 'ios' ? 'Courier' : 'monospace',
    marginTop: 3,
  },
  inputContainer: {
    flex: 1,
    marginLeft: 10,
  },
  tagInput: {
    backgroundColor: '#0F172A',
    color: '#F8FAFC',
    fontSize: 13,
    fontWeight: '600',
    paddingHorizontal: 12,
    paddingVertical: 8,
    borderRadius: 6,
    borderWidth: 1,
    borderColor: '#334155',
    letterSpacing: 0.5,
  },
  footerContainer: {
    paddingHorizontal: 20,
    paddingTop: 12,
    paddingBottom: Platform.OS === 'ios' ? 16 : 20,
    backgroundColor: '#0F172A',
    borderTopWidth: 1,
    borderTopColor: '#1E293B',
  },
  saveButton: {
    backgroundColor: '#00F5D4', // Botão de Ação Primária Cyan/Teal
    paddingVertical: 14,
    borderRadius: 8,
    alignItems: 'center',
    justifyContent: 'center',
    shadowColor: '#00F5D4',
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.3,
    shadowRadius: 4,
    elevation: 4,
  },
  saveButtonDisabled: {
    backgroundColor: '#0D9488',
    opacity: 0.7,
  },
  saveButtonText: {
    color: '#0F172A',
    fontSize: 15,
    fontWeight: '800',
    letterSpacing: 0.5,
  },
});

export default IOMappingScreen;
