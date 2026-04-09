<?php

require_once __DIR__ . '/src/UserService.php';

use App\Services\UserService;

function bootstrap(): void
{
    $service = new UserService();
    $user = $service->findById('u-1');
    echo "hello, {$user->name}\n";
}

bootstrap();
