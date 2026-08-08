<?php
/**
 * OBS WebSocket v5 PHP Relay & API Proxy
 * Relays commands and status queries between Web Frontend and OBS WebSocket v5 locally.
 * Zero external dependencies (pure PHP 7.4+ / 8.x).
 */

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With');

if (isset($_SERVER['REQUEST_METHOD']) && $_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

// Configuration file path
$configFile = __DIR__ . '/obs_config.json';
$defaultConfig = [
    'obs_host' => '127.0.0.1',
    'obs_port' => 4455,
    'obs_password' => '',
    'plugin_port' => 8080,
    'timeout' => 3
];

$config = $defaultConfig;
if (file_exists($configFile)) {
    $saved = @json_decode(file_get_contents($configFile), true);
    if (is_array($saved)) {
        $config = array_merge($defaultConfig, $saved);
    }
}

class ObsWebSocketClient {
    private $socket = null;
    private $host;
    private $port;
    private $password;
    private $timeout;
    private $authenticated = false;

    public function __construct($host = '127.0.0.1', $port = 4455, $password = '', $timeout = 3) {
        $this->host = $host;
        $this->port = (int)$port;
        $this->password = (string)$password;
        $this->timeout = (float)$timeout;
    }

    public function __destruct() {
        $this->close();
    }

    public function connect() {
        if ($this->socket) return true;

        $errno = 0;
        $errstr = '';
        $addr = "tcp://{$this->host}:{$this->port}";
        
        $context = stream_context_create([
            'socket' => [
                'connect_timeout' => $this->timeout
            ]
        ]);

        $this->socket = @stream_socket_client($addr, $errno, $errstr, $this->timeout, STREAM_CLIENT_CONNECT, $context);
        if (!$this->socket) {
            throw new Exception("Unable to connect to OBS WebSocket at {$addr}: {$errstr} ({$errno})");
        }

        stream_set_timeout($this->socket, (int)$this->timeout, (int)(($this->timeout - (int)$this->timeout) * 1000000));

        // Perform WebSocket handshake (RFC 6455)
        $key = base64_encode(random_bytes(16));
        $headers = "GET / HTTP/1.1\r\n" .
                   "Host: {$this->host}:{$this->port}\r\n" .
                   "Upgrade: websocket\r\n" .
                   "Connection: Upgrade\r\n" .
                   "Sec-WebSocket-Key: {$key}\r\n" .
                   "Sec-WebSocket-Version: 13\r\n\r\n";

        fwrite($this->socket, $headers);

        $response = '';
        while (!feof($this->socket)) {
            $line = fgets($this->socket, 1024);
            $response .= $line;
            if (rtrim($line) === '') break;
        }

        if (strpos($response, ' 101 ') === false) {
            $this->close();
            throw new Exception("WebSocket handshake failed. Response: " . trim($response));
        }

        // Handle OBS WebSocket v5 Initial Handshake (OpCode 0: Hello)
        $helloMsg = $this->readFrame();
        if (!$helloMsg || !isset($helloMsg['op']) || $helloMsg['op'] !== 0) {
            $this->close();
            throw new Exception("Expected OpCode 0 (Hello) from OBS WebSocket, received: " . json_encode($helloMsg));
        }

        // OpCode 1: Identify
        $identifyData = [
            'rpcVersion' => 1,
            'eventSubscriptions' => 0
        ];

        if (isset($helloMsg['d']['authentication'])) {
            $authInfo = $helloMsg['d']['authentication'];
            $challenge = $authInfo['challenge'];
            $salt = $authInfo['salt'];

            // OBS v5 Auth:
            // 1. base64_secret = base64_encode(sha256_binary(password + salt))
            // 2. auth_response = base64_encode(sha256_binary(base64_secret + challenge))
            $secret = base64_encode(hash('sha256', $this->password . $salt, true));
            $authResponse = base64_encode(hash('sha256', $secret . $challenge, true));
            $identifyData['authentication'] = $authResponse;
        }

        $this->writeFrame([
            'op' => 1,
            'd' => $identifyData
        ]);

        // OpCode 2: Identified
        $identifiedMsg = $this->readFrame();
        if (!$identifiedMsg || !isset($identifiedMsg['op']) || $identifiedMsg['op'] !== 2) {
            $this->close();
            $err = isset($identifiedMsg['d']['error']) ? $identifiedMsg['d']['error'] : 'Authentication / Identification failed';
            throw new Exception("OBS WebSocket Identification failed: {$err}");
        }

        $this->authenticated = true;
        return true;
    }

    public function request($requestType, $requestData = []) {
        $this->connect();
        $requestId = uniqid('req_', true);

        $this->writeFrame([
            'op' => 6,
            'd' => [
                'requestType' => $requestType,
                'requestId' => $requestId,
                'requestData' => (object)$requestData
            ]
        ]);

        $maxAttempts = 10;
        while ($maxAttempts-- > 0) {
            $msg = $this->readFrame();
            if (!$msg) break;
            if (isset($msg['op']) && $msg['op'] === 7 && isset($msg['d']['requestId']) && $msg['d']['requestId'] === $requestId) {
                if (isset($msg['d']['requestStatus']['result']) && $msg['d']['requestStatus']['result'] === true) {
                    return $msg['d']['responseData'] ?? [];
                } else {
                    $reason = $msg['d']['requestStatus']['comment'] ?? 'Unknown error';
                    $code = $msg['d']['requestStatus']['code'] ?? 0;
                    throw new Exception("OBS Request {$requestType} failed: {$reason} (Code {$code})");
                }
            }
        }
        throw new Exception("Timed out waiting for response to {$requestType}");
    }

    public function requestBatch(array $requests, $haltOnFailure = false, $executionType = 0) {
        $this->connect();
        $requestId = uniqid('batch_', true);

        $formattedRequests = [];
        foreach ($requests as $idx => $req) {
            $formattedRequests[] = [
                'requestType' => $req['requestType'],
                'requestId' => $req['requestId'] ?? "sub_{$idx}",
                'requestData' => (object)($req['requestData'] ?? [])
            ];
        }

        $this->writeFrame([
            'op' => 8,
            'd' => [
                'requestId' => $requestId,
                'haltOnFailure' => $haltOnFailure,
                'executionType' => $executionType,
                'requests' => $formattedRequests
            ]
        ]);

        $maxAttempts = 10;
        while ($maxAttempts-- > 0) {
            $msg = $this->readFrame();
            if (!$msg) break;
            if (isset($msg['op']) && $msg['op'] === 9 && isset($msg['d']['requestId']) && $msg['d']['requestId'] === $requestId) {
                return $msg['d']['results'] ?? [];
            }
        }
        throw new Exception("Timed out waiting for batch response");
    }

    private function writeFrame($data) {
        if (!$this->socket) return;
        $payload = json_encode($data);
        $length = strlen($payload);

        $frame = chr(0x81); // FIN + Text frame
        $mask = random_bytes(4);

        if ($length <= 125) {
            $frame .= chr(0x80 | $length);
        } elseif ($length <= 65535) {
            $frame .= chr(0x80 | 126) . pack('n', $length);
        } else {
            $frame .= chr(0x80 | 127) . pack('J', $length);
        }

        $frame .= $mask;
        $maskedPayload = '';
        for ($i = 0; $i < $length; $i++) {
            $maskedPayload .= $payload[$i] ^ $mask[$i % 4];
        }
        $frame .= $maskedPayload;

        @fwrite($this->socket, $frame);
    }

    private function readFrame() {
        if (!$this->socket || feof($this->socket)) return null;

        $header = @fread($this->socket, 2);
        if (!$header || strlen($header) < 2) return null;

        $byte0 = ord($header[0]);
        $byte1 = ord($header[1]);

        $opcode = $byte0 & 0x0F;
        $isMasked = ($byte1 & 0x80) === 0x80;
        $payloadLen = $byte1 & 0x7F;

        if ($payloadLen === 126) {
            $ext = @fread($this->socket, 2);
            if (strlen($ext) < 2) return null;
            $payloadLen = unpack('n', $ext)[1];
        } elseif ($payloadLen === 127) {
            $ext = @fread($this->socket, 8);
            if (strlen($ext) < 8) return null;
            $payloadLen = unpack('J', $ext)[1];
        }

        $mask = '';
        if ($isMasked) {
            $mask = @fread($this->socket, 4);
            if (strlen($mask) < 4) return null;
        }

        $payload = '';
        $remaining = $payloadLen;
        while ($remaining > 0 && !feof($this->socket)) {
            $chunk = @fread($this->socket, min(8192, $remaining));
            if ($chunk === false || strlen($chunk) === 0) break;
            $payload .= $chunk;
            $remaining -= strlen($chunk);
        }

        if ($isMasked && strlen($mask) === 4) {
            $unmasked = '';
            for ($i = 0; $i < strlen($payload); $i++) {
                $unmasked .= $payload[$i] ^ $mask[$i % 4];
            }
            $payload = $unmasked;
        }

        // Close frame
        if ($opcode === 0x08) {
            $this->close();
            return null;
        }

        // Ping frame -> Send Pong
        if ($opcode === 0x09) {
            $pongFrame = chr(0x8A) . chr(0x80 | strlen($payload)) . random_bytes(4);
            @fwrite($this->socket, $pongFrame);
            return $this->readFrame();
        }

        return json_decode($payload, true);
    }

    public function close() {
        if ($this->socket) {
            @fclose($this->socket);
            $this->socket = null;
        }
        $this->authenticated = false;
    }
}

// Request dispatcher
$rawInput = file_get_contents('php://input');
$inputData = @json_decode($rawInput, true) ?: [];
$action = $_GET['action'] ?? $inputData['action'] ?? 'get_overview';

// Helper to send json responses
function sendResponse($data, $statusCode = 200) {
    if (!headers_sent()) {
        http_response_code($statusCode);
    }
    echo json_encode($data, JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    exit;
}

function sendError($message, $statusCode = 500) {
    if (!headers_sent()) {
        http_response_code($statusCode);
    }
    echo json_encode(['status' => 'error', 'message' => $message]);
    exit;
}

// Plugin API Proxy helper
function proxyPluginApi($path, $method = 'GET', $bodyData = null, $pluginPort = 8080) {
    $url = "http://127.0.0.1:{$pluginPort}" . $path;
    $opts = [
        'http' => [
            'method' => $method,
            'header' => "Content-Type: application/json\r\n",
            'timeout' => 2,
            'ignore_errors' => true
        ]
    ];
    if ($bodyData !== null) {
        $opts['http']['content'] = json_encode($bodyData);
    }
    $context = stream_context_create($opts);
    $res = @file_get_contents($url, false, $context);
    if ($res === false) {
        return ['status' => 'error', 'message' => 'Plugin HTTP server unreachable'];
    }
    $decoded = json_decode($res, true);
    return $decoded !== null ? $decoded : ['status' => 'raw', 'data' => $res];
}

try {
    // 1. Config management
    if ($action === 'get_config') {
        sendResponse([
            'status' => 'ok',
            'config' => [
                'obs_host' => $config['obs_host'],
                'obs_port' => $config['obs_port'],
                'obs_password_set' => !empty($config['obs_password']),
                'plugin_port' => $config['plugin_port']
            ]
        ]);
    }

    if ($action === 'set_config') {
        if (!empty($inputData['obs_host'])) $config['obs_host'] = (string)$inputData['obs_host'];
        if (!empty($inputData['obs_port'])) $config['obs_port'] = (int)$inputData['obs_port'];
        if (isset($inputData['obs_password'])) $config['obs_password'] = (string)$inputData['obs_password'];
        if (!empty($inputData['plugin_port'])) $config['plugin_port'] = (int)$inputData['plugin_port'];
        @file_put_contents($configFile, json_encode($config, JSON_PRETTY_PRINT));
        sendResponse(['status' => 'ok', 'message' => 'Config updated successfully']);
    }

    // 2. Plugin Proxy Endpoints
    if ($action === 'plugin_settings') {
        $method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
        $res = proxyPluginApi('/api/settings', $method, $method === 'POST' ? $inputData : null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_receivers') {
        $res = proxyPluginApi('/api/receivers', 'GET', null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_stats') {
        $res = proxyPluginApi('/api/stats', 'GET', null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_receiver_action') {
        $res = proxyPluginApi('/api/receivers/action', 'POST', $inputData, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_multistream_targets') {
        $res = proxyPluginApi('/api/multistream/targets', 'GET', null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_multistream_action') {
        $res = proxyPluginApi('/api/multistream/action', 'POST', $inputData, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_multistream_manage') {
        $res = proxyPluginApi('/api/multistream/manage', 'POST', $inputData, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_autoswitch') {
        $method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
        $res = proxyPluginApi('/api/autoswitch', $method, $method === 'POST' ? $inputData : null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_stream_key') {
        $method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
        $res = proxyPluginApi('/api/stream_key', $method, $method === 'POST' ? $inputData : null, $config['plugin_port']);
        sendResponse($res);
    }

    if ($action === 'plugin_restart') {
        $res = proxyPluginApi('/api/restart', 'POST', [], $config['plugin_port']);
        sendResponse($res);
    }

    // 3. OBS WebSocket Client instantiation
    $client = new ObsWebSocketClient($config['obs_host'], $config['obs_port'], $config['obs_password'], $config['timeout']);

    // 4. Action Handlers
    switch ($action) {
        case 'get_overview':
            // Batch request for high speed in a single round trip
            $batchResults = $client->requestBatch([
                ['requestType' => 'GetSceneList', 'requestId' => 'scenes'],
                ['requestType' => 'GetStreamStatus', 'requestId' => 'stream'],
                ['requestType' => 'GetVirtualCamStatus', 'requestId' => 'vcam'],
                ['requestType' => 'GetStudioModeEnabled', 'requestId' => 'studio'],
                ['requestType' => 'GetReplayBufferStatus', 'requestId' => 'replay'],
                ['requestType' => 'GetSceneTransitionList', 'requestId' => 'transitions']
            ]);

            $overview = [
                'status' => 'ok',
                'connected' => true,
                'scenes' => [],
                'current_program_scene' => '',
                'stream_status' => [],
                'vcam_active' => false,
                'studio_mode_enabled' => false,
                'replay_buffer_active' => false,
                'transitions' => [],
                'current_transition' => '',
                'scene_items' => []
            ];

            foreach ($batchResults as $res) {
                $id = $res['requestId'] ?? '';
                $data = $res['responseData'] ?? [];
                if ($id === 'scenes') {
                    $overview['scenes'] = $data['scenes'] ?? [];
                    $overview['current_program_scene'] = $data['currentProgramSceneName'] ?? '';
                } elseif ($id === 'stream') {
                    $overview['stream_status'] = $data;
                } elseif ($id === 'vcam') {
                    $overview['vcam_active'] = $data['outputActive'] ?? false;
                } elseif ($id === 'studio') {
                    $overview['studio_mode_enabled'] = $data['studioModeEnabled'] ?? false;
                } elseif ($id === 'replay') {
                    $overview['replay_buffer_active'] = $data['outputActive'] ?? false;
                } elseif ($id === 'transitions') {
                    $overview['transitions'] = $data['transitions'] ?? [];
                    $overview['current_transition'] = $data['currentSceneTransitionName'] ?? '';
                }
            }

            // Also fetch scene items for the current scene
            if (!empty($overview['current_program_scene'])) {
                try {
                    $itemRes = $client->request('GetSceneItemList', ['sceneName' => $overview['current_program_scene']]);
                    $overview['scene_items'] = $itemRes['sceneItems'] ?? [];
                } catch (Exception $e) {
                    $overview['scene_items'] = [];
                }
            }

            sendResponse($overview);
            break;

        case 'get_scene_list':
            $res = $client->request('GetSceneList');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'get_scene_items':
            $sceneName = $_GET['sceneName'] ?? $inputData['sceneName'] ?? '';
            $res = $client->request('GetSceneItemList', ['sceneName' => $sceneName]);
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_current_scene':
            $sceneName = $inputData['sceneName'] ?? $_GET['sceneName'] ?? '';
            if (empty($sceneName)) sendError('sceneName is required', 400);
            $client->request('SetCurrentProgramScene', ['sceneName' => $sceneName]);
            sendResponse(['status' => 'ok', 'sceneName' => $sceneName]);
            break;

        case 'set_scene_item_enabled':
            $sceneName = $inputData['sceneName'] ?? '';
            $sceneItemId = (int)($inputData['sceneItemId'] ?? 0);
            $enabled = (bool)($inputData['sceneItemEnabled'] ?? false);
            if (empty($sceneName) || $sceneItemId <= 0) sendError('sceneName and sceneItemId required', 400);
            $client->request('SetSceneItemEnabled', [
                'sceneName' => $sceneName,
                'sceneItemId' => $sceneItemId,
                'sceneItemEnabled' => $enabled
            ]);
            sendResponse(['status' => 'ok', 'sceneItemId' => $sceneItemId, 'enabled' => $enabled]);
            break;

        case 'toggle_stream':
            $res = $client->request('ToggleStream');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'get_stream_status':
            $res = $client->request('GetStreamStatus');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'get_screenshot':
            $sourceName = $_GET['sourceName'] ?? $inputData['sourceName'] ?? '';
            $width = (int)($_GET['width'] ?? $inputData['width'] ?? 640);
            $height = (int)($_GET['height'] ?? $inputData['height'] ?? 360);
            $quality = (int)($_GET['quality'] ?? $inputData['quality'] ?? 50);

            $req = [
                'imageFormat' => 'jpeg',
                'imageWidth' => $width,
                'imageHeight' => $height,
                'imageCompressionQuality' => $quality
            ];
            if (!empty($sourceName)) {
                $req['sourceName'] = $sourceName;
            }

            $res = $client->request('GetSourceScreenshot', $req);
            sendResponse(['status' => 'ok', 'imageData' => $res['imageData'] ?? '']);
            break;

        case 'get_inputs':
            $res = $client->request('GetInputList');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'get_input_settings':
            $inputName = $_GET['inputName'] ?? $inputData['inputName'] ?? '';
            if (empty($inputName)) sendError('inputName is required', 400);
            $res = $client->request('GetInputSettings', ['inputName' => $inputName]);
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_input_settings':
            $inputName = $inputData['inputName'] ?? '';
            $settings = $inputData['inputSettings'] ?? [];
            if (empty($inputName)) sendError('inputName is required', 400);
            $client->request('SetInputSettings', [
                'inputName' => $inputName,
                'inputSettings' => $settings,
                'overlay' => true
            ]);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_video_settings':
            $res = $client->request('GetVideoSettings');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_video_settings':
            $client->request('SetVideoSettings', $inputData);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_record_directory':
            $res = $client->request('GetRecordDirectory');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_record_directory':
            $dir = $inputData['recordDirectory'] ?? '';
            $client->request('SetRecordDirectory', ['recordDirectory' => $dir]);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_profiles':
            $res = $client->request('GetProfileList');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_profile':
            $profileName = $inputData['profileName'] ?? '';
            $client->request('SetCurrentProfile', ['profileName' => $profileName]);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_scene_collections':
            $res = $client->request('GetSceneCollectionList');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_scene_collection':
            $name = $inputData['sceneCollectionName'] ?? '';
            $client->request('SetCurrentSceneCollection', ['sceneCollectionName' => $name]);
            sendResponse(['status' => 'ok']);
            break;

        case 'toggle_feature':
            $feature = $inputData['feature'] ?? $_GET['feature'] ?? '';
            $state = (bool)($inputData['state'] ?? $_GET['state'] ?? false);
            if ($feature === 'vcam') {
                $client->request($state ? 'StartVirtualCam' : 'StopVirtualCam');
            } elseif ($feature === 'replay') {
                $client->request($state ? 'StartReplayBuffer' : 'StopReplayBuffer');
            } elseif ($feature === 'studio') {
                $client->request('SetStudioModeEnabled', ['studioModeEnabled' => $state]);
            }
            sendResponse(['status' => 'ok', 'feature' => $feature, 'state' => $state]);
            break;

        case 'get_transitions':
            $res = $client->request('GetSceneTransitionList');
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_transition':
            $name = $inputData['transitionName'] ?? '';
            $client->request('SetCurrentSceneTransition', ['transitionName' => $name]);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_transform':
            $sceneName = $_GET['sceneName'] ?? $inputData['sceneName'] ?? '';
            $sceneItemId = (int)($_GET['sceneItemId'] ?? $inputData['sceneItemId'] ?? 0);
            $res = $client->request('GetSceneItemTransform', [
                'sceneName' => $sceneName,
                'sceneItemId' => $sceneItemId
            ]);
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_transform':
            $sceneName = $inputData['sceneName'] ?? '';
            $sceneItemId = (int)($inputData['sceneItemId'] ?? 0);
            $transform = $inputData['sceneItemTransform'] ?? [];
            $client->request('SetSceneItemTransform', [
                'sceneName' => $sceneName,
                'sceneItemId' => $sceneItemId,
                'sceneItemTransform' => $transform
            ]);
            sendResponse(['status' => 'ok']);
            break;

        case 'get_filters':
            $sourceName = $_GET['sourceName'] ?? $inputData['sourceName'] ?? '';
            $res = $client->request('GetSourceFilterList', ['sourceName' => $sourceName]);
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'get_filter_settings':
            $sourceName = $_GET['sourceName'] ?? $inputData['sourceName'] ?? '';
            $filterName = $_GET['filterName'] ?? $inputData['filterName'] ?? '';
            $res = $client->request('GetSourceFilter', [
                'sourceName' => $sourceName,
                'filterName' => $filterName
            ]);
            sendResponse(['status' => 'ok', 'data' => $res]);
            break;

        case 'set_filter_settings':
            $sourceName = $inputData['sourceName'] ?? '';
            $filterName = $inputData['filterName'] ?? '';
            $settings = $inputData['filterSettings'] ?? [];
            $client->request('SetSourceFilterSettings', [
                'sourceName' => $sourceName,
                'filterName' => $filterName,
                'filterSettings' => $settings
            ]);
            sendResponse(['status' => 'ok']);
            break;

        case 'execute':
            $requestType = $inputData['requestType'] ?? '';
            $requestData = $inputData['requestData'] ?? [];
            if (empty($requestType)) sendError('requestType required', 400);
            $res = $client->request($requestType, $requestData);
            sendResponse(['status' => 'ok', 'responseData' => $res]);
            break;

        default:
            sendError("Unknown action: {$action}", 400);
    }
} catch (Exception $e) {
    sendResponse([
        'status' => 'error',
        'connected' => false,
        'message' => $e->getMessage()
    ], 200); // Send 200 so UI can gracefully parse error message
}
