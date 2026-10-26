#!/usr/bin/env python3
"""ADR-0057: normal scripted CLI use, orderly reopen and public diagnostics."""
import os
import pathlib
import selectors
import signal
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


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

    def session(self, directory, commands, *args):
        env = os.environ.copy()
        env['XDG_CONFIG_HOME'] = str(pathlib.Path(directory) / 'config')
        env['XDG_DATA_HOME'] = str(pathlib.Path(directory) / 'data')
        return subprocess.run([BINARY, '--mock', *args], input='\n'.join(commands) + '\n',
                              text=True, capture_output=True, timeout=25, env=env)

    def test_two_agents_summary_and_reopen(self):
        with tempfile.TemporaryDirectory() as directory:
            commands = []
            for agent, capability in [('a', 'present'), ('b', 'model'), ('b', 'summary'), ('b', 'present')]:
                commands += [f'/grant {agent} {capability}', '/wait']
            commands += ['@a check', '/wait']
            for turn in range(1, 5):
                commands += [f'@b 中文第{turn}轮', '/wait']
            commands += ['/goals', '/tasks', '/calls', '/conversation', '/quit']
            first = self.session(directory, commands)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertIn('A：规则检查完成', first.stdout)
            self.assertIn('未确认摘要', first.stdout)
            self.assertIn('中文第4轮', first.stdout)
            self.assertIn('已完成', first.stdout)
            self.assertIn('Call ', first.stdout)
            second = self.session(directory, ['/conversation', '@b 重开后的第五轮', '/wait', '/status', '/quit'])
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn('中文第4轮', second.stdout)
            self.assertIn('对话轮次=5', second.stdout)
            self.assertIn('boot=2', second.stdout)
            state = pathlib.Path(directory) / 'data' / 'ayni'
            self.assertEqual(state.stat().st_mode & 0o777, 0o700)
            self.assertEqual((state / 'conversation.bin').stat().st_mode & 0o777, 0o600)

    def test_explicit_directory_and_eof_close(self):
        with tempfile.TemporaryDirectory() as directory:
            state = str(pathlib.Path(directory) / 'explicit')
            first = self.session(directory, ['@b 等待批准', '/wait', '/goals'], '--data-dir', state)
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
            result = self.session(directory, ['/grant b model surplus', '/status', '/quit'])
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


if __name__ == '__main__':
    unittest.main()
