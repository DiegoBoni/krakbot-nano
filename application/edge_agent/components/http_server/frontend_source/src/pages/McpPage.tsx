import { createMemo, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import { fetchMcpServers, saveMcpServer, testMcpServer } from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Switch } from '../components/ui/Switch';
import { Banner } from '../components/ui/Banner';
import { TextInput } from '../components/ui/FormField';
import { LabelLink } from '../components/ui/LabelLink';
import { pushToast } from '../state/toast';

const COMPOSIO_ID = 'composio';
const COMPOSIO_NAME = 'Composio';
const COMPOSIO_URL = 'https://connect.composio.dev/mcp';
const COMPOSIO_ENDPOINT = 'mcp';
const COMPOSIO_SIGNUP_URL = 'https://composio.dev';

export const McpPage: Component = () => {
  const [loading, setLoading] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);
  const [apiKey, setApiKey] = createSignal('');
  const [enabled, setEnabled] = createSignal(true);
  const [configured, setConfigured] = createSignal(false);
  const [saving, setSaving] = createSignal(false);
  const [testing, setTesting] = createSignal(false);

  const dirty = createMemo(() => apiKey().trim().length > 0);

  const load = async () => {
    setError(null);
    setLoading(true);
    try {
      const items = await fetchMcpServers();
      const existing = items.find((item) => item.id === COMPOSIO_ID);
      if (existing) {
        setApiKey(existing.secret);
        setEnabled(existing.enabled);
        setConfigured(true);
      } else {
        setConfigured(false);
      }
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  };
  load();

  const handleSave = async () => {
    const key = apiKey().trim();
    if (!key) {
      pushToast(t('mcpKeyRequired') as string, 'error');
      return;
    }
    setSaving(true);
    try {
      await saveMcpServer({
        id: COMPOSIO_ID,
        name: COMPOSIO_NAME,
        server_url: COMPOSIO_URL,
        endpoint: COMPOSIO_ENDPOINT,
        auth_type: 'bearer',
        username: '',
        secret: key,
        enabled: enabled(),
      });
      pushToast(t('mcpSaved') as string, 'success');
      setConfigured(true);
    } catch (err) {
      pushToast((err as Error).message, 'error');
    } finally {
      setSaving(false);
    }
  };

  const handleTest = async () => {
    setTesting(true);
    try {
      const result = await testMcpServer(COMPOSIO_ID);
      pushToast(result.message, result.ok ? 'success' : 'error', 5000);
    } catch (err) {
      pushToast((err as Error).message, 'error');
    } finally {
      setTesting(false);
    }
  };

  return (
    <TabShell>
      <PageHeader title={t('navMcp') as string} description={t('mcpDescription') as string} />
      <div class="p-5 flex flex-col gap-4 max-w-xl">
        <Banner kind="info" message={t('mcpUsageHint') as string} />
        <Show when={error()}>
          <Banner kind="error" message={error() ?? undefined} />
        </Show>
        <Show when={!loading()}>
          <div class="flex flex-col gap-3">
            <TextInput
              full
              type="password"
              label={
                <>
                  {t('mcpApiKeyLabel')}
                  <LabelLink href={COMPOSIO_SIGNUP_URL}>{t('mcpGetApiKey') as string} ↗</LabelLink>
                </>
              }
              placeholder="ck_..."
              value={apiKey()}
              onInput={(event) => setApiKey(event.currentTarget.value)}
            />
            <p class="text-[0.78rem] text-[var(--color-text-muted)] m-0">
              {t('mcpApiKeyHint')}
            </p>
            <Switch
              label={t('mcpFieldEnabled') as string}
              checked={enabled()}
              onChange={(checked) => setEnabled(checked)}
            />
            <div class="flex gap-2 pt-2">
              <Button variant="primary" disabled={saving() || !dirty()} onClick={() => handleSave()}>
                {saving() ? t('mcpSaving') : t('mcpSave')}
              </Button>
              <Show when={configured()}>
                <Button variant="secondary" disabled={testing()} onClick={() => handleTest()}>
                  {testing() ? t('mcpTesting') : t('mcpTestConnection')}
                </Button>
              </Show>
            </div>
          </div>
        </Show>
      </div>
    </TabShell>
  );
};
