import { Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextInput, SelectInput } from '../components/ui/FormField';
import { LabelLink } from '../components/ui/LabelLink';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { GROQ_API_KEY_URL } from '../constants/externalLinks';

type VoiceForm = {
  asr_api_key: string;
  asr_model: string;
  tts_api_key: string;
  tts_voice: string;
  tts_model: string;
  tts_provider: string;
};

type TtsGender = 'female' | 'male';

const TTS_GENDER_VOICES: Record<string, Record<TtsGender, string>> = {
  openai: { female: 'nova', male: 'onyx' },
  groq: { female: 'hannah', male: 'troy' },
};

function detectGender(provider: string, voice: string): TtsGender | '' {
  const trimmed = voice.trim().toLowerCase();
  if (!trimmed) return '';
  const presets = TTS_GENDER_VOICES[provider];
  if (presets && trimmed === presets.female) return 'female';
  if (presets && trimmed === presets.male) return 'male';
  return '';
}

export const VoicePage: Component = () => {
  const tab = createConfigTab<VoiceForm>({
    tab: 'voice',
    groups: ['asr', 'tts'],
    toForm: (config: Partial<AppConfig>) => ({
      asr_api_key: config.asr_api_key ?? '',
      asr_model: config.asr_model ?? '',
      tts_api_key: config.tts_api_key ?? '',
      tts_voice: config.tts_voice ?? '',
      tts_model: config.tts_model ?? '',
      tts_provider: config.tts_provider ?? 'openai',
    }),
    fromForm: (form) => ({
      asr_api_key: form.asr_api_key.trim(),
      asr_model: form.asr_model.trim(),
      tts_api_key: form.tts_api_key.trim(),
      tts_voice: form.tts_voice.trim(),
      tts_model: form.tts_model.trim(),
      tts_provider: form.tts_provider.trim(),
    }),
  });

  const selectGender = (gender: TtsGender) => {
    const preset = TTS_GENDER_VOICES[tab.form.tts_provider]?.[gender];
    if (preset) tab.setForm('tts_voice', preset);
  };

  return (
    <TabShell>
      <PageHeader title={t('navVoice') as string} />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionAsrTranscription') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <TextInput
              type="password"
              label={
                <>
                  {t('asrApiKey')}
                  <LabelLink href={GROQ_API_KEY_URL}>
                    {t('llmProviderConsole') as string} ↗
                  </LabelLink>
                </>
              }
              value={tab.form.asr_api_key}
              onInput={(event) => tab.setForm('asr_api_key', event.currentTarget.value)}
            />
            <TextInput
              label={t('asrModel')}
              placeholder={t('asrModelPlaceholder') as string}
              value={tab.form.asr_model}
              onInput={(event) => tab.setForm('asr_model', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">
            {t('asrTranscriptionNote')}
          </p>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionTtsVoice') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <SelectInput
              label={t('ttsProvider')}
              value={tab.form.tts_provider}
              onChange={(event) => tab.setForm('tts_provider', event.currentTarget.value)}
            >
              <option value="openai">{t('ttsProviderOpenai') as string}</option>
              <option value="groq">{t('ttsProviderGroq') as string}</option>
            </SelectInput>
            <div />
            <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 sm:col-span-2">
              {tab.form.tts_provider === 'groq' ? t('ttsProviderGroqNote') : t('ttsProviderOpenaiNote')}
            </p>
            <div class="sm:col-span-2">
              <span class="block text-[0.82rem] text-[var(--color-text-secondary)] mb-1.5">
                {t('ttsGender')}
              </span>
              <div class="flex gap-2">
                <button
                  type="button"
                  onClick={() => selectGender('female')}
                  class={[
                    'px-3 py-1.5 rounded-[var(--radius-sm)] text-[0.82rem] border transition',
                    detectGender(tab.form.tts_provider, tab.form.tts_voice) === 'female'
                      ? 'bg-[var(--color-accent)]/15 border-[var(--color-accent-soft)]/60 text-[var(--color-text-primary)]'
                      : 'border-[var(--color-border-subtle)] text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)]',
                  ].join(' ')}
                >
                  {t('ttsGenderFemale')}
                </button>
                <button
                  type="button"
                  onClick={() => selectGender('male')}
                  class={[
                    'px-3 py-1.5 rounded-[var(--radius-sm)] text-[0.82rem] border transition',
                    detectGender(tab.form.tts_provider, tab.form.tts_voice) === 'male'
                      ? 'bg-[var(--color-accent)]/15 border-[var(--color-accent-soft)]/60 text-[var(--color-text-primary)]'
                      : 'border-[var(--color-border-subtle)] text-[var(--color-text-secondary)] hover:text-[var(--color-text-primary)]',
                  ].join(' ')}
                >
                  {t('ttsGenderMale')}
                </button>
              </div>
              <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-1.5">
                {t('ttsGenderNote')}
              </p>
            </div>
            <TextInput
              label={t('ttsVoice')}
              placeholder={
                tab.form.tts_provider === 'groq' ? 'autumn / diana / hannah / austin / daniel / troy' : (t('ttsVoicePlaceholder') as string)
              }
              value={tab.form.tts_voice}
              onInput={(event) => tab.setForm('tts_voice', event.currentTarget.value)}
            />
            <TextInput
              label={t('ttsModel')}
              placeholder={
                tab.form.tts_provider === 'groq' ? 'canopylabs/orpheus-v1-english' : (t('ttsModelPlaceholder') as string)
              }
              value={tab.form.tts_model}
              onInput={(event) => tab.setForm('tts_model', event.currentTarget.value)}
            />
          </div>
          <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0 pt-3">
            {t('ttsVoiceNote')}
          </p>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty()}
        saving={tab.saving()}
        onSave={() => tab.save().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
    </TabShell>
  );
};
