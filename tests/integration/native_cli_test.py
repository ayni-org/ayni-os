#!/usr/bin/env python3
"""ADR-0057/0072: normal scripted CLI use, orderly reopen and public diagnostics."""
import os
import fcntl
import pathlib
import selectors
import signal
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)
# conversation.bin starts with b'AYNF' and then its little-endian format version.
FORMAT_VERSION_OFFSET = 4


class NativeCli(unittest.TestCase):
    def test_sigint_while_waiting_for_input_closes_normally(self):
        # ADR-0057: input remains interruptible while the owner thread advances.
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / 'config'
            config.write_text('provider=mock\n')
            with subprocess.Popen([BINARY, '--config', str(config), '--data-dir', directory + '/state'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True) as process:
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(process.stdout, selectors.EVENT_READ)
                        self.assertTrue(selector.select(timeout=15), 'CLI startup timed out')
                    self.assertIn('Ayni Native CLI', process.stdout.readline())
                    process.send_signal(signal.SIGINT)
                    stdout, stderr = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, stdout + stderr)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()

    @unittest.skipUnless(hasattr(fcntl, "F_SETPIPE_SZ"), "Linux pipe sizing")
    def test_stdout_backpressure_keeps_command_processing_and_reports_failed_drain(self):
        # ADR-0066: ordinary console output also stays outside the Kernel owner.
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / 'config'
            config.write_text('provider=mock\n')
            with subprocess.Popen([BINARY, '--config', str(config), '--data-dir', directory + '/state'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True) as process:
                try:
                    fcntl.fcntl(process.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
                    process.stdin.write('/status\n' * 200 + '/quit\n')
                    process.stdin.close()
                    # Do not drain stdout until quit has been processed. A blocking
                    # owner or normal-exit flush would exceed this finite deadline.
                    process.wait(timeout=12)
                    self.assertEqual(process.returncode, 1)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()

    def session(self, directory, commands, *args):
        env = os.environ.copy()
        env['XDG_CONFIG_HOME'] = str(pathlib.Path(directory) / 'config')
        env['XDG_DATA_HOME'] = str(pathlib.Path(directory) / 'data')
        return subprocess.run([BINARY, '--mock', *args], input='\n'.join(commands) + '\n',
                              text=True, capture_output=True, timeout=25, env=env)

    def test_single_assistant_short_history_and_reopen(self):
        # ADR-0072: every plain line, including "check", is a turn of the one ToolLoop assistant.
        with tempfile.TemporaryDirectory() as directory:
            commands = []
            for capability in ['model', 'summary', 'schedule']:
                commands += [f'/grant {capability}', '/wait']
            commands += ['check', '/wait']
            for turn in range(2, 5):
                commands += [f'中文第{turn}轮', '/wait']
            commands += ['/goals', '/tasks', '/calls', '/conversation', '/quit']
            first = self.session(directory, commands)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            goals = [line for line in first.stdout.splitlines() if line.startswith('Goal ')]
            self.assertEqual(len(goals), 4, first.stdout)
            self.assertTrue(all('已完成' in line for line in goals), first.stdout)
            self.assertNotIn('未确认摘要', first.stdout)
            self.assertIn('中文第4轮', first.stdout)
            self.assertIn('Call ', first.stdout)
            second = self.session(directory, ['/conversation', '重开后的第五轮', '/wait', '/diagnostics', '/quit'])
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn('中文第4轮', second.stdout)
            self.assertIn('对话轮次=5', second.stdout)
            self.assertIn('boot=2', second.stdout)
            state = pathlib.Path(directory) / 'data' / 'ayni'
            self.assertEqual(state.stat().st_mode & 0o777, 0o700)
            self.assertEqual((state / 'conversation.sqlite3').stat().st_mode & 0o777, 0o600)

    def test_stop_phrase_pauses_background_work_without_the_model(self):
        # ADR-0072: the phrase is handled before any grant or model call exists.
        with tempfile.TemporaryDirectory() as directory:
            result = self.session(directory, ['停！', '/status', '/resume-assistant', '/wait', '/status', '/quit'])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('已停止：进行中的回答（如有）已取消，后台工作已暂停，提醒保留', result.stdout)
            lines = [line for line in result.stdout.splitlines() if '助理：' in line and '处理' in line]
            self.assertEqual(len(lines), 2, result.stdout)
            self.assertIn('自主工作暂停', lines[0])
            self.assertNotIn('自主工作暂停', lines[1])
            self.assertNotIn('你：', result.stdout)

    def test_dual_agent_directory_is_refused_unchanged(self):
        # ADR-0072: the earlier layout is refused at startup, never migrated.
        with tempfile.TemporaryDirectory() as directory:
            first = self.session(directory, ['/quit'])
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            state = pathlib.Path(directory) / 'data' / 'ayni'
            conversation = state / 'conversation.bin'
            legacy = bytearray(b'AYNF\x02\x00\x00\x00legacy-directory')
            legacy[FORMAT_VERSION_OFFSET] = 2  # AYNF v2, written by the dual-Agent version.
            conversation.write_bytes(legacy)
            before = {path.relative_to(state): path.read_bytes() for path in state.rglob('*') if path.is_file()}
            second = self.session(directory, ['/quit'])
            self.assertEqual(second.returncode, 1, second.stdout + second.stderr)
            self.assertIn('不兼容的旧版本', second.stdout)
            self.assertNotIn('Ayni Native CLI', second.stdout)
            after = {path.relative_to(state): path.read_bytes() for path in state.rglob('*') if path.is_file()}
            self.assertEqual(before, after)

    def test_explicit_directory_and_eof_close(self):
        with tempfile.TemporaryDirectory() as directory:
            state = str(pathlib.Path(directory) / 'explicit')
            first = self.session(directory, ['等待批准', '/wait', '/goals'], '--data-dir', state)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertIn('待审批', first.stdout)
            goal_line = next(line for line in first.stdout.splitlines() if line.startswith('Goal '))
            goal_id = goal_line.split()[1]
            second = self.session(directory, [f'/approve {goal_id}', '/wait', '/conversation', '/quit'],
                                  '--data-dir', state)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn('等待批准', second.stdout)

    def test_invalid_command_is_reported_without_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.session(directory, ['/grant model surplus', '/diagnostics', '/quit'])
            self.assertEqual(result.returncode, 1)
            self.assertIn('ERROR 命令格式无效', result.stdout)
            self.assertIn('对话轮次=0', result.stdout)

    def test_unknown_secret_key_configuration_does_not_echo_value(self):
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / 'config-file'
            config.write_text('api_key=do-not-print-this-fixture\n')
            result = self.session(directory, [], '--config', str(config))
            self.assertEqual(result.returncode, 2)
            self.assertNotIn('do-not-print-this-fixture', result.stdout + result.stderr)

    def test_timezone_configuration_takes_only_a_posix_rule(self):
        # ADR-0073: the zone stated to the model must be a whole POSIX rule, never a zone name.
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / 'config-file'
            config.write_text('timezone=Asia/Shanghai\n')
            refused = self.session(directory, [], '--config', str(config))
            self.assertEqual(refused.returncode, 2)
            self.assertIn('timezone 须为 POSIX 时区规则', refused.stderr + refused.stdout)
            config.write_text('timezone=CST-8\n')
            accepted = self.session(directory, ['/quit'], '--config', str(config))
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)


if __name__ == '__main__':
    unittest.main()
